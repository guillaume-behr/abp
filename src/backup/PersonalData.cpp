#include "abp/PersonalData.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <system_error>

#include "abp/ArchiveIntegrity.h"
#include "abp/FsUtil.h"
#include "abp/Json.h"
#include "abp/Logger.h"
#include "abp/StringUtil.h"

namespace abp::personal {
namespace fs = std::filesystem;
namespace {

using json::JsonValue;

constexpr const char* kExportDir = "personal";
constexpr const char* kMmsPartsDir = "mms_parts";

/// Lookup keys handed to one `as_multi_vcard` read. The keys go into the URI,
/// and the URI into the shell command, so this bounds the command length.
constexpr size_t kContactsPerVcardRead = 25;

std::string nullToEmpty(std::string value) { return value == "NULL" ? std::string() : value; }

/// Runs `command` (a `content ...` or `settings ...` invocation) on the
/// device. Returns false, with the reason in `*why`, if it was refused or
/// failed; these tools report that on stderr (or as an "Error while
/// accessing provider" line) and not always through their exit status.
bool runDeviceTool(const AdbClient& adb, const std::string& command, std::string* output, std::string* why) {
    ProcessResult r = adb.shell(command);
    *output = r.stdOut;
    const std::string out = strutil::trim(r.stdOut);
    const std::string err = strutil::trim(r.stdErr);

    std::string problem;
    if (r.spawnFailed) {
        problem = "could not run adb";
    } else if (strutil::startsWith(out, "Error while") || err.find("Exception") != std::string::npos ||
               err.find("Error while") != std::string::npos) {
        problem = err.empty() ? out : err;
    } else if (!r.ok() && out.empty()) {
        problem = err.empty() ? "the device refused or does not support it" : err;
    }
    if (problem.empty()) return true;

    // The useful line is the exception ("... Permission Denial: ... requires
    // android.permission.READ_SMS"); the rest is a Java stack trace.
    for (const auto& line : strutil::split(problem, '\n')) {
        const std::string trimmed = strutil::trim(line);
        if (trimmed.find("Exception") != std::string::npos || trimmed.find("Denial") != std::string::npos) {
            problem = trimmed;
            break;
        }
    }
    *why = problem.substr(0, 300);
    return false;
}

bool queryProvider(const AdbClient& adb, const std::string& uri, const std::vector<std::string>& columns,
                   std::vector<Row>* rows, std::string* why, const std::string& where = std::string()) {
    std::string command = "content query --uri " + strutil::shellQuote(uri) + " --projection " +
                          strutil::shellQuote(strutil::join(columns, ":"));
    if (!where.empty()) command += " --where " + strutil::shellQuote(where);
    std::string output;
    if (!runDeviceTool(adb, command, &output, why)) return false;
    *rows = parseContentQuery(output, columns);
    return true;
}

enum class UtcStyle { Iso, IcsDateTime, IcsDate };

std::string formatUtc(std::time_t seconds, UtcStyle style) {
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[40];
    switch (style) {
        case UtcStyle::Iso: std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc); break;
        case UtcStyle::IcsDateTime: std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc); break;
        case UtcStyle::IcsDate: std::strftime(buffer, sizeof(buffer), "%Y%m%d", &utc); break;
    }
    return buffer;
}

long long toInt(const std::string& text) {
    try {
        return std::stoll(text);
    } catch (const std::exception&) {
        return 0;
    }
}

std::string isoFromMillis(long long millis) {
    return millis > 0 ? formatUtc(static_cast<std::time_t>(millis / 1000), UtcStyle::Iso) : std::string();
}

/// Writes `content` to `personal/<fileName>` and records it in the manifest.
bool recordExport(const fs::path& outDir, const std::string& fileName, const std::string& content,
                  const std::string& kind, const std::string& format, int count, Manifest& manifest,
                  bool ownerOnly = false) {
    const fs::path path = outDir / kExportDir / fileName;
    try {
        fsutil::writeTextFile(path, content);
    } catch (const std::exception& e) {
        Logger::warn(std::string("Could not write ") + path.string() + ": " + e.what());
        return false;
    }
    if (ownerOnly) {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    }
    PersonalDataExport item;
    item.kind = kind;
    item.format = format;
    item.localPath = (fs::path(kExportDir) / fileName).generic_string();
    item.itemCount = count;
    item.bytes = fsutil::fileSize(path);
    item.sha256 = integrity::checksumOrEmpty(path);
    manifest.personalDataExports.push_back(item);
    return true;
}

ExportResult skipped(const std::string& kind, const std::string& label, const std::string& reason) {
    Logger::warn(label + " not exported: " + reason);
    return ExportResult{kind, label, -1, reason};
}

ExportResult exported(const std::string& kind, const std::string& label, int count) {
    return ExportResult{kind, label, count, std::string()};
}

int countOccurrences(const std::string& text, const std::string& needle) {
    int count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

// ------------------------------------------------------------------ contacts

/// Contacts, preferably as the provider's own vCard export (which keeps every
/// field, photos included), falling back to a vCard assembled from the raw
/// data rows when this device's `content` tool has no `read` command.
ExportResult exportContacts(const AdbClient& adb, const fs::path& outDir, Manifest& manifest) {
    const char* label = "Contacts";
    std::vector<Row> contacts;
    std::string why;
    if (!queryProvider(adb, "content://com.android.contacts/contacts", {"_id", "lookup"}, &contacts, &why)) {
        return skipped("contacts", label, why);
    }

    std::string vcards;
    bool native = !contacts.empty();
    for (size_t i = 0; native && i < contacts.size(); i += kContactsPerVcardRead) {
        std::vector<std::string> keys;
        for (size_t j = i; j < contacts.size() && j < i + kContactsPerVcardRead; ++j) {
            if (!contacts[j]["lookup"].empty()) keys.push_back(contacts[j]["lookup"]);
        }
        if (keys.empty()) continue;
        const std::string uri = "content://com.android.contacts/contacts/as_multi_vcard/" +
                                encodeUriComponent(strutil::join(keys, ":"));
        std::string chunk;
        std::string readWhy;
        if (!runDeviceTool(adb, "content read --uri " + strutil::shellQuote(uri), &chunk, &readWhy) ||
            !strutil::startsWith(strutil::trim(chunk), "BEGIN:VCARD")) {
            native = false;
            break;
        }
        vcards += chunk;
        if (!vcards.empty() && vcards.back() != '\n') vcards += "\r\n";
    }

    int count = 0;
    if (native) {
        count = countOccurrences(vcards, "BEGIN:VCARD");
    } else if (!contacts.empty()) {
        Logger::debug("The device could not stream vCards; building them from the contacts data table.");
        std::vector<Row> dataRows;
        if (!queryProvider(adb, "content://com.android.contacts/data",
                           {"contact_id", "mimetype", "data2", "data3", "data1"}, &dataRows, &why)) {
            return skipped("contacts", label, why);
        }
        vcards = buildVcards(dataRows, &count);
    }

    if (!recordExport(outDir, "contacts.vcf", vcards, "contacts", "vcard", count, manifest)) {
        return skipped("contacts", label, "could not write the export file");
    }
    return exported("contacts", label, count);
}

// ----------------------------------------------------------- SMS, call log

const char* smsTypeName(long long type) {
    switch (type) {
        case 1: return "inbox";
        case 2: return "sent";
        case 3: return "draft";
        case 4: return "outbox";
        case 5: return "failed";
        case 6: return "queued";
        default: return "other";
    }
}

ExportResult exportSms(const AdbClient& adb, const fs::path& outDir, const std::string& exportedAt,
                       Manifest& manifest) {
    const char* label = "SMS messages";
    // `body` last: it is the only free-text column (see parseContentQuery).
    const std::vector<std::string> columns = {"_id", "thread_id", "address", "date", "date_sent", "type", "read",
                                              "body"};
    std::vector<Row> rows;
    std::string why;
    if (!queryProvider(adb, "content://sms", columns, &rows, &why)) return skipped("sms", label, why);

    JsonValue messages = JsonValue::makeArray();
    for (auto& row : rows) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", toInt(row["_id"]));
        item.set("thread_id", toInt(row["thread_id"]));
        item.set("address", row["address"]);
        item.set("date", toInt(row["date"]));
        item.set("date_utc", isoFromMillis(toInt(row["date"])));
        item.set("date_sent", toInt(row["date_sent"]));
        item.set("type", smsTypeName(toInt(row["type"])));
        item.set("read", row["read"] == "1");
        item.set("body", row["body"]);
        messages.push_back(item);
    }

    JsonValue doc = JsonValue::makeObject();
    doc.set("exported_at_utc", exportedAt);
    doc.set("source", "content://sms");
    doc.set("messages", messages);
    const int count = static_cast<int>(rows.size());
    if (!recordExport(outDir, "sms.json", doc.dump(2) + "\n", "sms", "json", count, manifest)) {
        return skipped("sms", label, "could not write the export file");
    }
    return exported("sms", label, count);
}

const char* callTypeName(long long type) {
    switch (type) {
        case 1: return "incoming";
        case 2: return "outgoing";
        case 3: return "missed";
        case 4: return "voicemail";
        case 5: return "rejected";
        case 6: return "blocked";
        default: return "other";
    }
}

ExportResult exportCallLog(const AdbClient& adb, const fs::path& outDir, const std::string& exportedAt,
                           Manifest& manifest) {
    const char* label = "Call log";
    const std::vector<std::string> columns = {"_id", "number", "type", "date", "duration", "name"};
    std::vector<Row> rows;
    std::string why;
    if (!queryProvider(adb, "content://call_log/calls", columns, &rows, &why)) return skipped("call_log", label, why);

    JsonValue calls = JsonValue::makeArray();
    for (auto& row : rows) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", toInt(row["_id"]));
        item.set("number", row["number"]);
        item.set("name", row["name"]);
        item.set("type", callTypeName(toInt(row["type"])));
        item.set("date", toInt(row["date"]));
        item.set("date_utc", isoFromMillis(toInt(row["date"])));
        item.set("duration_seconds", toInt(row["duration"]));
        calls.push_back(item);
    }

    JsonValue doc = JsonValue::makeObject();
    doc.set("exported_at_utc", exportedAt);
    doc.set("source", "content://call_log/calls");
    doc.set("calls", calls);
    const int count = static_cast<int>(rows.size());
    if (!recordExport(outDir, "call_log.json", doc.dump(2) + "\n", "call_log", "json", count, manifest)) {
        return skipped("call_log", label, "could not write the export file");
    }
    return exported("call_log", label, count);
}

// ----------------------------------------------------------------------- MMS

const char* mmsBoxName(long long box) {
    switch (box) {
        case 1: return "inbox";
        case 2: return "sent";
        case 3: return "draft";
        case 4: return "outbox";
        default: return "other";
    }
}

std::string extensionForContentType(const std::string& type) {
    static const std::map<std::string, std::string> kKnown = {
        {"image/jpeg", ".jpg"}, {"image/jpg", ".jpg"},  {"image/png", ".png"},   {"image/gif", ".gif"},
        {"image/webp", ".webp"}, {"image/heic", ".heic"}, {"video/mp4", ".mp4"},  {"video/3gpp", ".3gp"},
        {"audio/amr", ".amr"},   {"audio/mpeg", ".mp3"},  {"audio/mp4", ".m4a"},  {"text/x-vcard", ".vcf"},
        {"text/vcard", ".vcf"},  {"text/x-vcalendar", ".vcs"},
    };
    auto it = kKnown.find(type);
    return it == kKnown.end() ? std::string(".bin") : it->second;
}

/// MMS (picture and group messages): headers from content://mms, text and
/// attachments from content://mms/part, and each conversation's participants
/// from the threads table -- the per-message address table would cost one
/// query (a JVM start on the device) per message.
ExportResult exportMms(const AdbClient& adb, const fs::path& outDir, const std::string& exportedAt,
                       Manifest& manifest) {
    const char* label = "MMS messages";
    std::vector<Row> messagesRows;
    std::string why;
    if (!queryProvider(adb, "content://mms", {"_id", "thread_id", "date", "msg_box", "sub"}, &messagesRows, &why)) {
        return skipped("mms", label, why);
    }

    std::vector<Row> parts;
    if (!messagesRows.empty() &&
        !queryProvider(adb, "content://mms/part", {"_id", "mid", "seq", "ct", "name", "text"}, &parts, &why)) {
        Logger::warn("MMS text and attachments not exported: " + why);
        parts.clear();
    }

    // thread_id -> participant addresses. Best-effort: without it the
    // messages are still exported, just without who they were with.
    std::map<std::string, std::vector<std::string>> participants;
    std::vector<Row> threads;
    std::vector<Row> addresses;
    std::string ignored;
    if (!messagesRows.empty() &&
        queryProvider(adb, "content://mms-sms/conversations?simple=true", {"_id", "recipient_ids"}, &threads,
                      &ignored) &&
        queryProvider(adb, "content://mms-sms/canonical-addresses", {"_id", "address"}, &addresses, &ignored)) {
        std::map<std::string, std::string> addressById;
        for (auto& row : addresses) addressById[row["_id"]] = row["address"];
        for (auto& row : threads) {
            for (const auto& id : strutil::split(row["recipient_ids"], ' ')) {
                auto it = addressById.find(strutil::trim(id));
                if (it != addressById.end() && !it->second.empty()) participants[row["_id"]].push_back(it->second);
            }
        }
    }

    const fs::path partsDir = outDir / kExportDir / kMmsPartsDir;
    int attachments = 0;
    bool attachmentsWork = true;
    std::map<std::string, JsonValue> partsByMessage;
    for (auto& part : parts) {
        const std::string& type = part["ct"];
        if (type == "application/smil") continue; // Layout only.
        JsonValue item = JsonValue::makeObject();
        item.set("content_type", type);
        if (!part["name"].empty()) item.set("name", part["name"]);
        if (type == "text/plain") {
            item.set("text", part["text"]);
        } else if (attachmentsWork) {
            // Attachments are binary; stream each straight into a file.
            fsutil::ensureDirectory(partsDir);
            const std::string fileName = fsutil::sanitizeForFilename(part["_id"]) + extensionForContentType(type);
            const fs::path local = partsDir / fileName;
            const std::string uri = "content://mms/part/" + part["_id"];
            if (adb.execOutToFile("content read --uri " + strutil::shellQuote(uri), local.string()) &&
                fsutil::fileSize(local) > 0) {
                item.set("file", (fs::path(kMmsPartsDir) / fileName).generic_string());
                ++attachments;
            } else {
                std::error_code ec;
                fs::remove(local, ec);
                if (attachments == 0) {
                    // The first one failing means `content read` is missing or
                    // refused; do not spend a device round trip on every other.
                    attachmentsWork = false;
                    Logger::warn("MMS attachments could not be read from this device; exporting their text only.");
                }
            }
        }
        auto [it, inserted] = partsByMessage.emplace(part["mid"], JsonValue::makeArray());
        (void)inserted;
        it->second.push_back(item);
    }

    JsonValue messages = JsonValue::makeArray();
    for (auto& row : messagesRows) {
        JsonValue item = JsonValue::makeObject();
        const long long seconds = toInt(row["date"]); // MMS dates are in seconds, unlike SMS.
        item.set("id", toInt(row["_id"]));
        item.set("thread_id", toInt(row["thread_id"]));
        item.set("date", seconds * 1000);
        item.set("date_utc", isoFromMillis(seconds * 1000));
        item.set("box", mmsBoxName(toInt(row["msg_box"])));
        item.set("subject", row["sub"]);
        JsonValue people = JsonValue::makeArray();
        for (const auto& address : participants[row["thread_id"]]) people.push_back(address);
        item.set("participants", people);
        auto partsIt = partsByMessage.find(row["_id"]);
        item.set("parts", partsIt == partsByMessage.end() ? JsonValue::makeArray() : partsIt->second);
        messages.push_back(item);
    }

    JsonValue doc = JsonValue::makeObject();
    doc.set("exported_at_utc", exportedAt);
    doc.set("source", "content://mms");
    doc.set("note", "participants lists everyone in the conversation; attachment files are under mms_parts/.");
    doc.set("messages", messages);
    const int count = static_cast<int>(messagesRows.size());
    if (!recordExport(outDir, "mms.json", doc.dump(2) + "\n", "mms", "json", count, manifest)) {
        return skipped("mms", label, "could not write the export file");
    }
    if (attachments > 0) Logger::info("Saved " + std::to_string(attachments) + " MMS attachment(s).");
    return exported("mms", label, count);
}

// ------------------------------------------------------------------ calendar

ExportResult exportCalendar(const AdbClient& adb, const fs::path& outDir, Manifest& manifest) {
    const char* label = "Calendar events";
    std::vector<Row> calendars;
    std::string why;
    if (!queryProvider(adb, "content://com.android.calendar/calendars", {"_id", "calendar_displayName"}, &calendars,
                       &why)) {
        return skipped("calendar", label, why);
    }
    std::map<std::string, std::string> names;
    for (auto& row : calendars) names[row["_id"]] = row["calendar_displayName"];

    // `description` last: it is the longest free-text column.
    std::vector<Row> events;
    if (!queryProvider(adb, "content://com.android.calendar/events",
                       {"_id", "calendar_id", "dtstart", "dtend", "allDay", "rrule", "duration", "eventTimezone",
                        "eventLocation", "title", "description"},
                       &events, &why, "deleted=0")) {
        return skipped("calendar", label, why);
    }

    int count = 0;
    const std::string ics = buildIcs(events, names, &count);
    if (!recordExport(outDir, "calendar.ics", ics, "calendar", "icalendar", count, manifest)) {
        return skipped("calendar", label, "could not write the export file");
    }
    return exported("calendar", label, count);
}

// ------------------------------------------------------------------ settings

ExportResult exportSettings(const AdbClient& adb, const fs::path& outDir, const std::string& exportedAt,
                            Manifest& manifest) {
    const char* label = "Settings";
    JsonValue doc = JsonValue::makeObject();
    doc.set("exported_at_utc", exportedAt);
    doc.set("note", "Archival: abp does not write settings back, since many are device-specific.");
    int count = 0;
    std::string lastWhy;
    for (const char* ns : {"system", "secure", "global"}) {
        std::string output;
        std::string why;
        if (!runDeviceTool(adb, std::string("settings list ") + ns, &output, &why)) {
            lastWhy = why;
            continue;
        }
        JsonValue values = JsonValue::makeObject();
        for (const auto& entry : parseSettingsList(output)) {
            values.set(entry.first, entry.second);
            ++count;
        }
        doc.set(ns, values);
    }
    if (count == 0) return skipped("settings", label, lastWhy.empty() ? "the device listed no settings" : lastWhy);
    if (!recordExport(outDir, "settings.json", doc.dump(2) + "\n", "settings", "json", count, manifest)) {
        return skipped("settings", label, "could not write the export file");
    }
    return exported("settings", label, count);
}

// --------------------------------------------------------------------- Wi-Fi

std::string asRoot(const RootAccess& root, const std::string& command) {
    return root.method == RootMethod::SuBinary ? "su -c " + strutil::shellQuote(command) : command;
}

ExportResult exportWifi(const AdbClient& adb, const fs::path& outDir, const DeviceInfo& device,
                        Manifest& manifest) {
    const char* label = "Wi-Fi networks";
    if (!device.isRooted()) {
        // The shell user can list saved networks but never their passwords,
        // which is the part worth saving.
        return ExportResult{"wifi", label, -1, "needs root (the passwords are only readable as root)"};
    }

    // Where each Android generation keeps the store, newest first.
    struct Source {
        const char* path;
        bool supplicant;
    };
    const Source sources[] = {
        {"/data/misc/apexdata/com.android.wifi/WifiConfigStore.xml", false}, // Android 11+
        {"/data/misc/wifi/WifiConfigStore.xml", false},                      // Android 8-10
        {"/data/misc/wifi/wpa_supplicant.conf", true},                       // Android 7 and older
    };
    for (const auto& source : sources) {
        bool ok = false;
        const std::string content =
            adb.shellText(asRoot(device.root, "cat " + strutil::shellQuote(source.path) + " 2>/dev/null"), &ok);
        if (!ok || content.empty()) continue;

        const std::vector<WifiNetwork> networks =
            source.supplicant ? parseWpaSupplicant(content) : parseWifiConfigStore(content);
        JsonValue list = JsonValue::makeArray();
        for (const auto& network : networks) {
            JsonValue item = JsonValue::makeObject();
            item.set("ssid", network.ssid);
            item.set("security", network.security);
            item.set("password", network.password);
            item.set("hidden", network.hidden);
            list.push_back(item);
        }
        JsonValue doc = JsonValue::makeObject();
        doc.set("source", source.path);
        doc.set("networks", list);

        const int count = static_cast<int>(networks.size());
        // Passwords in clear text: keep both files readable by the owner only.
        const std::string rawName = fs::path(source.path).filename().string();
        if (!recordExport(outDir, "wifi.json", doc.dump(2) + "\n", "wifi", "json", count, manifest, true)) {
            return skipped("wifi", label, "could not write the export file");
        }
        try {
            const fs::path raw = outDir / kExportDir / ("wifi_" + rawName);
            fsutil::writeTextFile(raw, content + "\n");
            std::error_code ec;
            fs::permissions(raw, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        } catch (const std::exception&) {
            // The parsed wifi.json is the part that matters.
        }
        Logger::warn("personal/wifi.json holds Wi-Fi passwords in clear text; keep this backup private.");
        return exported("wifi", label, count);
    }
    return skipped("wifi", label, "no Wi-Fi configuration store found on the device");
}

// ------------------------------------------------------------------- helpers

std::string vcardEscape(const std::string& value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case ',': out += "\\,"; break;
            case ';': out += "\\;"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            default: out.push_back(c);
        }
    }
    return out;
}

const char* phoneTypeName(const std::string& type) {
    if (type == "1") return "HOME";
    if (type == "2") return "CELL";
    if (type == "3") return "WORK";
    if (type == "4") return "WORK,FAX";
    if (type == "5") return "HOME,FAX";
    if (type == "6") return "PAGER";
    return "VOICE";
}

/// Splits an iCalendar content line into 75-octet lines (RFC 5545 3.1),
/// never inside a UTF-8 sequence.
std::string foldIcsLine(const std::string& line) {
    std::string out;
    size_t start = 0;
    size_t limit = 75;
    while (line.size() - start > limit) {
        size_t cut = start + limit;
        while (cut > start && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;
        out += line.substr(start, cut - start) + "\r\n ";
        start = cut;
        limit = 74; // Continuation lines start with a space.
    }
    out += line.substr(start) + "\r\n";
    return out;
}

std::string xmlUnescape(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        const size_t semi = text.find(';', i);
        if (semi == std::string::npos) {
            out.push_back('&');
            continue;
        }
        const std::string entity = text.substr(i + 1, semi - i - 1);
        if (entity == "quot") out.push_back('"');
        else if (entity == "amp") out.push_back('&');
        else if (entity == "lt") out.push_back('<');
        else if (entity == "gt") out.push_back('>');
        else if (entity == "apos") out.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            const long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                                  ? std::strtol(entity.c_str() + 2, nullptr, 16)
                                  : std::strtol(entity.c_str() + 1, nullptr, 10);
            if (code > 0 && code < 128) out.push_back(static_cast<char>(code));
        } else {
            out += text.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

/// Android stores SSIDs and passphrases in quotes; unquoted means raw hex.
std::string unquote(const std::string& value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
    return value;
}

std::string xmlStringValue(const std::string& block, const std::string& name) {
    const std::string open = "<string name=\"" + name + "\">";
    const size_t start = block.find(open);
    if (start == std::string::npos) return std::string();
    const size_t valueStart = start + open.size();
    const size_t end = block.find("</string>", valueStart);
    if (end == std::string::npos) return std::string();
    return xmlUnescape(block.substr(valueStart, end - valueStart));
}

bool xmlBoolValue(const std::string& block, const std::string& name) {
    const size_t pos = block.find("<boolean name=\"" + name + "\" value=\"true\"");
    return pos != std::string::npos;
}

std::string securityFromKeyManagement(const std::string& keyManagement) {
    if (keyManagement == "WPA_PSK" || keyManagement == "WPA-PSK") return "wpa2";
    if (keyManagement == "SAE") return "wpa3";
    if (keyManagement == "NONE") return "open";
    if (keyManagement == "OWE") return "owe";
    return keyManagement.empty() ? std::string("unknown") : keyManagement;
}

} // namespace

// --------------------------------------------------------------- public API

std::vector<Row> parseContentQuery(const std::string& output, const std::vector<std::string>& columns) {
    std::vector<Row> rows;
    if (columns.empty()) return rows;

    // A row starts at the beginning of a line with "Row: <n> <first>=", where
    // n counts up from 0. Requiring the next expected index (not just any
    // "Row: ") keeps a message body that happens to contain a line starting
    // with "Row: " from being split in two.
    std::vector<size_t> starts;
    for (size_t pos = 0; pos < output.size();) {
        const std::string marker = "Row: " + std::to_string(starts.size()) + " " + columns[0] + "=";
        const size_t found = output.find(marker, pos);
        if (found == std::string::npos) break;
        if (found == 0 || output[found - 1] == '\n') starts.push_back(found);
        pos = found + 1;
    }

    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t end = i + 1 < starts.size() ? starts[i + 1] : output.size();
        std::string text = output.substr(starts[i], end - starts[i]);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();

        // Skip "Row: <index> ".
        size_t cursor = text.find(' ', 5);
        if (cursor == std::string::npos) continue;
        ++cursor;

        Row row;
        bool valid = true;
        for (size_t c = 0; c < columns.size(); ++c) {
            const std::string key = columns[c] + "=";
            if (text.compare(cursor, key.size(), key) != 0) {
                valid = false;
                break;
            }
            cursor += key.size();
            size_t valueEnd = text.size();
            if (c + 1 < columns.size()) {
                valueEnd = text.find(", " + columns[c + 1] + "=", cursor);
                if (valueEnd == std::string::npos) {
                    valid = false;
                    break;
                }
            }
            row[columns[c]] = nullToEmpty(text.substr(cursor, valueEnd - cursor));
            cursor = valueEnd + 2; // Past ", ".
        }
        if (valid) rows.push_back(std::move(row));
    }
    return rows;
}

std::string encodeUriComponent(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string buildVcards(const std::vector<Row>& dataRows, int* count) {
    struct Contact {
        std::string displayName, given, family, organization, note;
        std::vector<std::pair<std::string, std::string>> phones; // type, number
        std::vector<std::string> emails, addresses;
    };
    std::vector<std::pair<std::string, Contact>> contacts; // In provider order.

    auto find = [&contacts](const std::string& id) -> Contact& {
        for (auto& entry : contacts) {
            if (entry.first == id) return entry.second;
        }
        contacts.emplace_back(id, Contact{});
        return contacts.back().second;
    };

    for (const auto& row : dataRows) {
        auto value = [&row](const char* key) {
            auto it = row.find(key);
            return it == row.end() ? std::string() : it->second;
        };
        const std::string id = value("contact_id");
        if (id.empty()) continue;
        const std::string mime = value("mimetype");
        Contact& contact = find(id);
        if (mime == "vnd.android.cursor.item/name") {
            contact.displayName = value("data1");
            contact.given = value("data2");
            contact.family = value("data3");
        } else if (mime == "vnd.android.cursor.item/phone_v2") {
            if (!value("data1").empty()) contact.phones.emplace_back(phoneTypeName(value("data2")), value("data1"));
        } else if (mime == "vnd.android.cursor.item/email_v2") {
            if (!value("data1").empty()) contact.emails.push_back(value("data1"));
        } else if (mime == "vnd.android.cursor.item/postal-address_v2") {
            if (!value("data1").empty()) contact.addresses.push_back(value("data1"));
        } else if (mime == "vnd.android.cursor.item/organization") {
            contact.organization = value("data1");
        } else if (mime == "vnd.android.cursor.item/note") {
            contact.note = value("data1");
        }
    }

    std::string out;
    int written = 0;
    for (const auto& entry : contacts) {
        const Contact& c = entry.second;
        std::string name = c.displayName;
        if (name.empty()) name = strutil::trim(c.given + " " + c.family);
        if (name.empty() && !c.phones.empty()) name = c.phones.front().second;
        if (name.empty() && !c.emails.empty()) name = c.emails.front();
        if (name.empty()) continue; // Nothing identifying at all.

        out += "BEGIN:VCARD\r\nVERSION:3.0\r\n";
        out += "FN:" + vcardEscape(name) + "\r\n";
        out += "N:" + vcardEscape(c.family) + ";" + vcardEscape(c.given) + ";;;\r\n";
        for (const auto& phone : c.phones) out += "TEL;TYPE=" + phone.first + ":" + vcardEscape(phone.second) + "\r\n";
        for (const auto& email : c.emails) out += "EMAIL;TYPE=INTERNET:" + vcardEscape(email) + "\r\n";
        for (const auto& address : c.addresses) out += "ADR:;;" + vcardEscape(address) + ";;;;\r\n";
        if (!c.organization.empty()) out += "ORG:" + vcardEscape(c.organization) + "\r\n";
        if (!c.note.empty()) out += "NOTE:" + vcardEscape(c.note) + "\r\n";
        out += "END:VCARD\r\n";
        ++written;
    }
    if (count != nullptr) *count = written;
    return out;
}

std::string buildIcs(const std::vector<Row>& events, const std::map<std::string, std::string>& calendarNames,
                     int* count) {
    auto value = [](const Row& row, const char* key) {
        auto it = row.find(key);
        return it == row.end() ? std::string() : it->second;
    };
    const std::string stamp = formatUtc(std::time(nullptr), UtcStyle::IcsDateTime);

    std::string out = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//abp//Android backup//EN\r\nCALSCALE:GREGORIAN\r\n";
    int written = 0;
    for (const auto& event : events) {
        const long long start = toInt(value(event, "dtstart"));
        if (start <= 0) continue;
        const long long end = toInt(value(event, "dtend"));
        const bool allDay = value(event, "allDay") == "1";

        std::string body = "BEGIN:VEVENT\r\n";
        body += foldIcsLine("UID:abp-" + value(event, "_id") + "-" + value(event, "calendar_id") + "@android");
        body += "DTSTAMP:" + stamp + "\r\n";
        if (allDay) {
            // All-day events are stored as UTC midnights.
            const std::time_t startSeconds = static_cast<std::time_t>(start / 1000);
            const std::time_t endSeconds = end > start ? static_cast<std::time_t>(end / 1000) : startSeconds + 86400;
            body += "DTSTART;VALUE=DATE:" + formatUtc(startSeconds, UtcStyle::IcsDate) + "\r\n";
            body += "DTEND;VALUE=DATE:" + formatUtc(endSeconds, UtcStyle::IcsDate) + "\r\n";
        } else {
            body += "DTSTART:" + formatUtc(static_cast<std::time_t>(start / 1000), UtcStyle::IcsDateTime) + "\r\n";
            if (end > start) {
                body += "DTEND:" + formatUtc(static_cast<std::time_t>(end / 1000), UtcStyle::IcsDateTime) + "\r\n";
            } else if (!value(event, "duration").empty()) {
                body += "DURATION:" + value(event, "duration") + "\r\n";
            }
        }
        if (!value(event, "rrule").empty()) body += foldIcsLine("RRULE:" + value(event, "rrule"));
        body += foldIcsLine("SUMMARY:" + vcardEscape(value(event, "title")));
        if (!value(event, "eventLocation").empty()) {
            body += foldIcsLine("LOCATION:" + vcardEscape(value(event, "eventLocation")));
        }
        if (!value(event, "description").empty()) {
            body += foldIcsLine("DESCRIPTION:" + vcardEscape(value(event, "description")));
        }
        auto name = calendarNames.find(value(event, "calendar_id"));
        if (name != calendarNames.end() && !name->second.empty()) {
            body += foldIcsLine("CATEGORIES:" + vcardEscape(name->second));
        }
        body += "END:VEVENT\r\n";
        out += body;
        ++written;
    }
    out += "END:VCALENDAR\r\n";
    if (count != nullptr) *count = written;
    return out;
}

std::map<std::string, std::string> parseSettingsList(const std::string& output) {
    std::map<std::string, std::string> values;
    for (const auto& rawLine : strutil::split(output, '\n')) {
        std::string line = rawLine;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t equals = line.find('=');
        if (equals == 0 || equals == std::string::npos) continue;
        values[line.substr(0, equals)] = line.substr(equals + 1);
    }
    return values;
}

std::vector<WifiNetwork> parseWifiConfigStore(const std::string& xml) {
    std::vector<WifiNetwork> networks;
    for (size_t pos = xml.find("<Network>"); pos != std::string::npos; pos = xml.find("<Network>", pos + 1)) {
        const size_t end = xml.find("</Network>", pos);
        if (end == std::string::npos) break;
        const std::string block = xml.substr(pos, end - pos);

        WifiNetwork network;
        network.ssid = unquote(xmlStringValue(block, "SSID"));
        if (network.ssid.empty()) continue;
        network.password = unquote(xmlStringValue(block, "PreSharedKey"));
        network.hidden = xmlBoolValue(block, "HiddenSSID");

        // ConfigKey is the quoted SSID followed by the key management, e.g.
        // "\"Home\"WPA_PSK" -- the most reliable place to read the security.
        const std::string configKey = xmlStringValue(block, "ConfigKey");
        const size_t lastQuote = configKey.rfind('"');
        std::string keyManagement = lastQuote == std::string::npos ? std::string() : configKey.substr(lastQuote + 1);
        if (keyManagement.empty()) keyManagement = network.password.empty() ? "NONE" : "WPA_PSK";
        network.security = securityFromKeyManagement(keyManagement);
        networks.push_back(network);
    }
    return networks;
}

std::vector<WifiNetwork> parseWpaSupplicant(const std::string& conf) {
    std::vector<WifiNetwork> networks;
    for (size_t pos = conf.find("network={"); pos != std::string::npos; pos = conf.find("network={", pos + 1)) {
        const size_t end = conf.find('}', pos);
        if (end == std::string::npos) break;
        WifiNetwork network;
        std::string keyManagement;
        for (const auto& rawLine : strutil::split(conf.substr(pos + 9, end - pos - 9), '\n')) {
            const std::string line = strutil::trim(rawLine);
            const size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            const std::string key = line.substr(0, equals);
            const std::string value = line.substr(equals + 1);
            if (key == "ssid") network.ssid = unquote(value);
            else if (key == "psk") network.password = unquote(value);
            else if (key == "key_mgmt") keyManagement = value;
            else if (key == "scan_ssid") network.hidden = value == "1";
        }
        if (network.ssid.empty()) continue;
        if (keyManagement.empty()) keyManagement = network.password.empty() ? "NONE" : "WPA-PSK";
        network.security = securityFromKeyManagement(keyManagement);
        networks.push_back(network);
    }
    return networks;
}

std::string wifiAddNetworkCommand(const WifiNetwork& network) {
    if (network.ssid.empty()) return std::string();
    const bool needsPassword = network.security == "wpa2" || network.security == "wpa3";
    if (!needsPassword && network.security != "open" && network.security != "owe") return std::string();
    if (needsPassword && network.password.empty()) return std::string();

    std::string command = "cmd wifi add-network " + strutil::shellQuote(network.ssid) + " " + network.security;
    if (needsPassword) command += " " + strutil::shellQuote(network.password);
    if (network.hidden) command += " -h";
    return command;
}

std::vector<ExportResult> exportPersonalData(const AdbClient& adb, const fs::path& outDir, const DeviceInfo& device,
                                             Manifest& manifest) {
    std::vector<ExportResult> results;
    if (!fsutil::ensureDirectory(outDir / kExportDir)) {
        Logger::warn("Could not create " + (outDir / kExportDir).string() + "; personal data not exported.");
        return results;
    }

    const std::string stamp = formatUtc(std::time(nullptr), UtcStyle::Iso);
    results.push_back(exportContacts(adb, outDir, manifest));
    results.push_back(exportSms(adb, outDir, stamp, manifest));
    results.push_back(exportMms(adb, outDir, stamp, manifest));
    results.push_back(exportCallLog(adb, outDir, stamp, manifest));
    results.push_back(exportCalendar(adb, outDir, manifest));
    results.push_back(exportSettings(adb, outDir, stamp, manifest));
    results.push_back(exportWifi(adb, outDir, device, manifest));

    std::error_code ec;
    if (fs::is_empty(outDir / kExportDir, ec)) fs::remove(outDir / kExportDir, ec);
    return results;
}

namespace {

const PersonalDataExport* findExport(const Manifest& manifest, const std::string& kind) {
    for (const auto& item : manifest.personalDataExports) {
        if (item.kind == kind) return &item;
    }
    return nullptr;
}

/// Copies an export to the device's Download folder, after checking it is
/// the file the backup recorded.
std::string pushForImport(const AdbClient& adb, const fs::path& backupDir, const PersonalDataExport& item,
                          const std::string& remoteName) {
    const fs::path local = backupDir / item.localPath;
    if (!fs::exists(local) || !integrity::checksumMatches(local, item.sha256)) {
        Logger::warn(item.localPath + " is missing or does not match its checksum; not copied to the device.");
        return std::string();
    }
    const std::string remote = "/sdcard/Download/" + remoteName;
    if (!adb.push(local.string(), remote)) {
        Logger::warn("Could not copy " + item.localPath + " to the device.");
        return std::string();
    }
    return remote;
}

} // namespace

RestoreResult restorePersonalData(const AdbClient& adb, const fs::path& backupDir, const Manifest& manifest,
                                  int sdkInt) {
    RestoreResult result;

    if (const auto* contacts = findExport(manifest, "contacts"); contacts != nullptr && contacts->itemCount > 0) {
        result.contactsPath = pushForImport(adb, backupDir, *contacts, "abp-contacts.vcf");
        if (!result.contactsPath.empty()) {
            Logger::info("Copied " + std::to_string(contacts->itemCount) + " contact(s) to " + result.contactsPath +
                         ". Import them in the Contacts app: Settings > Import > .vcf file.");
        }
    }

    if (const auto* calendar = findExport(manifest, "calendar"); calendar != nullptr && calendar->itemCount > 0) {
        result.calendarPath = pushForImport(adb, backupDir, *calendar, "abp-calendar.ics");
        if (!result.calendarPath.empty()) {
            Logger::info("Copied " + std::to_string(calendar->itemCount) + " calendar event(s) to " +
                         result.calendarPath +
                         ". Open it with your calendar app to import it (or import it into Google Calendar on "
                         "the web).");
        }
    }

    if (const auto* wifi = findExport(manifest, "wifi"); wifi != nullptr && wifi->itemCount > 0) {
        const fs::path local = backupDir / wifi->localPath;
        if (sdkInt < 30) {
            Logger::warn("Saved Wi-Fi networks can only be re-added on Android 11 or later; see " + wifi->localPath +
                         " for the passwords.");
        } else if (!integrity::checksumMatches(local, wifi->sha256)) {
            Logger::warn(wifi->localPath + " is missing or does not match its checksum; Wi-Fi networks not restored.");
        } else {
            result.wifiRestored = 0;
            try {
                JsonValue doc = JsonValue::parse(fsutil::readTextFile(local));
                // Bound to a name: get() returns by value, and iterating the
                // temporary's items would walk destroyed storage.
                const JsonValue networks = doc.get("networks");
                for (const auto& item : networks.items()) {
                    WifiNetwork network;
                    network.ssid = item.get("ssid").asString();
                    network.security = item.get("security").asString();
                    network.password = item.get("password").asString();
                    network.hidden = item.get("hidden").asBool();
                    const std::string command = wifiAddNetworkCommand(network);
                    if (!command.empty() && adb.shell(command).ok()) {
                        ++result.wifiRestored;
                    } else {
                        ++result.wifiSkipped;
                    }
                }
            } catch (const std::exception& e) {
                Logger::warn("Could not read " + wifi->localPath + ": " + e.what());
            }
            Logger::info("Re-added " + std::to_string(result.wifiRestored) + " Wi-Fi network(s)" +
                         (result.wifiSkipped > 0 ? "; " + std::to_string(result.wifiSkipped) +
                                                       " could not be (enterprise, WEP or unknown password)"
                                                 : std::string()) +
                         ".");
        }
    }

    for (const char* kind : {"sms", "mms", "call_log", "settings"}) {
        if (const auto* item = findExport(manifest, kind); item != nullptr) {
            Logger::info("The backup's " + item->localPath +
                         " is a readable archive; Android does not let abp write it back.");
        }
    }
    return result;
}

} // namespace abp::personal
