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

/// Lookup keys handed to one `as_multi_vcard` read. The keys go into the URI,
/// and the URI into the shell command, so this bounds the command length.
constexpr size_t kContactsPerVcardRead = 25;

std::string nullToEmpty(std::string value) { return value == "NULL" ? std::string() : value; }

/// Runs `content <args>` on the device. Returns false, with the reason in
/// `*why`, if the provider refused or the tool failed; the content tool
/// reports those on stderr (or as an "Error while accessing provider" line)
/// and not always through its exit status.
bool runContent(const AdbClient& adb, const std::string& args, std::string* output, std::string* why) {
    ProcessResult r = adb.shell("content " + args);
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
        problem = err.empty() ? "the 'content' tool failed" : err;
    }
    if (problem.empty()) return true;

    // The first line is the useful one ("... Permission Denial: ... requires
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
                   std::vector<Row>* rows, std::string* why) {
    std::string output;
    if (!runContent(adb, "query --uri " + strutil::shellQuote(uri) + " --projection " +
                             strutil::shellQuote(strutil::join(columns, ":")),
                    &output, why)) {
        return false;
    }
    *rows = parseContentQuery(output, columns);
    return true;
}

std::string isoFromMillis(const std::string& millisText) {
    long long millis = 0;
    try {
        millis = std::stoll(millisText);
    } catch (const std::exception&) {
        return std::string();
    }
    if (millis <= 0) return std::string();
    std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

long long toInt(const std::string& text) {
    try {
        return std::stoll(text);
    } catch (const std::exception&) {
        return 0;
    }
}

/// Writes `content` to `personal/<fileName>` and records it in the manifest.
bool recordExport(const fs::path& outDir, const std::string& fileName, const std::string& content,
                  const std::string& kind, const std::string& format, int count, Manifest& manifest) {
    const fs::path path = outDir / kExportDir / fileName;
    try {
        fsutil::writeTextFile(path, content);
    } catch (const std::exception& e) {
        Logger::warn(std::string("Could not write ") + path.string() + ": " + e.what());
        return false;
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

int countOccurrences(const std::string& text, const std::string& needle) {
    int count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

/// Contacts, preferably as the provider's own vCard export (which keeps every
/// field, photos included), falling back to a vCard assembled from the raw
/// data rows when this device's `content` tool has no `read` command.
int exportContacts(const AdbClient& adb, const fs::path& outDir, Manifest& manifest) {
    std::vector<Row> contacts;
    std::string why;
    if (!queryProvider(adb, "content://com.android.contacts/contacts", {"_id", "lookup"}, &contacts, &why)) {
        Logger::warn("Contacts not exported: " + why);
        return -1;
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
        if (!runContent(adb, "read --uri " + strutil::shellQuote(uri), &chunk, &readWhy) ||
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
            Logger::warn("Contacts not exported: " + why);
            return -1;
        }
        vcards = buildVcards(dataRows, &count);
    }

    if (!recordExport(outDir, "contacts.vcf", vcards, "contacts", "vcard", count, manifest)) return -1;
    return count;
}

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

int exportSms(const AdbClient& adb, const fs::path& outDir, const std::string& exportedAt, Manifest& manifest) {
    // `body` last: it is the only free-text column (see parseContentQuery).
    const std::vector<std::string> columns = {"_id", "thread_id", "address", "date", "date_sent", "type", "read",
                                              "body"};
    std::vector<Row> rows;
    std::string why;
    if (!queryProvider(adb, "content://sms", columns, &rows, &why)) {
        Logger::warn("SMS not exported: " + why);
        return -1;
    }

    JsonValue messages = JsonValue::makeArray();
    for (auto& row : rows) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", toInt(row["_id"]));
        item.set("thread_id", toInt(row["thread_id"]));
        item.set("address", row["address"]);
        item.set("date", toInt(row["date"]));
        item.set("date_utc", isoFromMillis(row["date"]));
        item.set("date_sent", toInt(row["date_sent"]));
        item.set("type", smsTypeName(toInt(row["type"])));
        item.set("read", row["read"] == "1");
        item.set("body", row["body"]);
        messages.push_back(item);
    }

    JsonValue doc = JsonValue::makeObject();
    doc.set("exported_at_utc", exportedAt);
    doc.set("source", "content://sms");
    doc.set("note", "SMS only; MMS (picture/group messages) are not included.");
    doc.set("messages", messages);
    if (!recordExport(outDir, "sms.json", doc.dump(2) + "\n", "sms", "json", static_cast<int>(rows.size()), manifest)) {
        return -1;
    }
    return static_cast<int>(rows.size());
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

int exportCallLog(const AdbClient& adb, const fs::path& outDir, const std::string& exportedAt, Manifest& manifest) {
    const std::vector<std::string> columns = {"_id", "number", "type", "date", "duration", "name"};
    std::vector<Row> rows;
    std::string why;
    if (!queryProvider(adb, "content://call_log/calls", columns, &rows, &why)) {
        Logger::warn("Call log not exported: " + why);
        return -1;
    }

    JsonValue calls = JsonValue::makeArray();
    for (auto& row : rows) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", toInt(row["_id"]));
        item.set("number", row["number"]);
        item.set("name", row["name"]);
        item.set("type", callTypeName(toInt(row["type"])));
        item.set("date", toInt(row["date"]));
        item.set("date_utc", isoFromMillis(row["date"]));
        item.set("duration_seconds", toInt(row["duration"]));
        calls.push_back(item);
    }

    JsonValue doc = JsonValue::makeObject();
    doc.set("exported_at_utc", exportedAt);
    doc.set("source", "content://call_log/calls");
    doc.set("calls", calls);
    if (!recordExport(outDir, "call_log.json", doc.dump(2) + "\n", "call_log", "json",
                      static_cast<int>(rows.size()), manifest)) {
        return -1;
    }
    return static_cast<int>(rows.size());
}

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

} // namespace

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

ExportCounts exportPersonalData(const AdbClient& adb, const fs::path& outDir, Manifest& manifest) {
    ExportCounts counts;
    if (!fsutil::ensureDirectory(outDir / kExportDir)) {
        Logger::warn("Could not create " + (outDir / kExportDir).string() + "; contacts and messages not exported.");
        return counts;
    }

    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);

    counts.contacts = exportContacts(adb, outDir, manifest);
    counts.sms = exportSms(adb, outDir, stamp, manifest);
    counts.callLog = exportCallLog(adb, outDir, stamp, manifest);

    std::error_code ec;
    if (fs::is_empty(outDir / kExportDir, ec)) fs::remove(outDir / kExportDir, ec);
    return counts;
}

std::string pushContactsForImport(const AdbClient& adb, const fs::path& backupDir, const Manifest& manifest) {
    for (const auto& item : manifest.personalDataExports) {
        if (item.kind != "contacts" || item.itemCount <= 0) continue;
        const fs::path local = backupDir / item.localPath;
        if (!integrity::checksumMatches(local, item.sha256)) {
            Logger::warn("Contacts export " + item.localPath + " is missing or does not match its checksum; not copied.");
            return std::string();
        }
        const std::string remote = "/sdcard/Download/abp-contacts.vcf";
        if (!adb.push(local.string(), remote)) {
            Logger::warn("Could not copy " + item.localPath + " to the device.");
            return std::string();
        }
        return remote;
    }
    return std::string();
}

} // namespace abp::personal
