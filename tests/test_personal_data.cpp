#include "abp/PersonalData.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "TestFramework.h"
#include "abp/FsUtil.h"
#include "abp/Json.h"
#include "abp/StringUtil.h"

using namespace abp;
using namespace abp::personal;
namespace fs = std::filesystem;

namespace {

/// A fake `adb` whose shell commands are matched by the script, plus a
/// scratch backup directory; both cleaned up on destruction.
class PersonalFixture {
public:
    explicit PersonalFixture(const std::string& script) : previousPath_(AdbClient::adbPath()) {
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("abp_personal_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(root_ / "backup");
        const fs::path adb = root_ / "adb";
        std::ofstream out(adb);
        out << "#!/bin/sh\necho \"$*\" >> \"$(dirname \"$0\")/calls.log\"\n"
            << "[ \"$1\" = \"-s\" ] && shift 2\n"
            << script;
        out.close();
        fs::permissions(adb, fs::perms::owner_all);
        AdbClient::setAdbPath(adb.string());
    }

    ~PersonalFixture() {
        AdbClient::setAdbPath(previousPath_);
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    PersonalFixture(const PersonalFixture&) = delete;
    PersonalFixture& operator=(const PersonalFixture&) = delete;

    fs::path backupDir() const { return root_ / "backup"; }
    std::string readLog() const {
        const fs::path log = root_ / "calls.log";
        return fs::exists(log) ? fsutil::readTextFile(log) : std::string();
    }

private:
    std::string previousPath_;
    fs::path root_;
};

/// A device that answers everything: two contacts streamed as native vCards,
/// three SMS (one with commas and a newline in its body), and a call log the
/// shell user is not allowed to read.
const char* kPhoneScript = R"SH(
[ "$1" = "shell" ] || exit 1
case "$2" in
  *"content://com.android.contacts/contacts'"*)
    echo "Row: 0 _id=1, lookup=0r1-ABC"
    echo "Row: 1 _id=2, lookup=0r2-DEF"
    exit 0;;
  *"read --uri"*"as_multi_vcard/0r1-ABC%3A0r2-DEF"*)
    printf 'BEGIN:VCARD\r\nVERSION:2.1\r\nFN:Ada\r\nEND:VCARD\r\nBEGIN:VCARD\r\nVERSION:2.1\r\nFN:Bob\r\nEND:VCARD\r\n'
    exit 0;;
  *"content://sms'"*)
    echo "Row: 0 _id=10, thread_id=1, address=+15550001, date=1700000000000, date_sent=0, type=1, read=1, body=Hi, see you at 5, ok?"
    echo "Row: 1 _id=11, thread_id=1, address=+15550001, date=1700000100000, date_sent=1700000099000, type=2, read=1, body=Line one"
    echo "Row: 7 is just text in the same message"
    echo "Row: 2 _id=12, thread_id=2, address=NULL, date=1700000200000, date_sent=0, type=3, read=0, body=NULL"
    exit 0;;
  *"call_log"*)
    echo "Error while accessing provider:call_log" >&2
    echo "java.lang.SecurityException: Permission Denial: opening provider requires android.permission.READ_CALL_LOG" >&2
    exit 1;;
esac
exit 1
)SH";

} // namespace

ABP_TEST(personal_parses_content_query_rows_by_projection) {
    const std::string output =
        "Row: 0 _id=1, address=+1555, body=Hello, world, date=not a column\n"
        "Row: 1 _id=2, address=NULL, body=multi\nline\n";
    auto rows = parseContentQuery(output, {"_id", "address", "body"});
    ABP_CHECK_EQ(rows.size(), 2u);
    ABP_CHECK_EQ(rows[0]["_id"], "1");
    ABP_CHECK_EQ(rows[0]["address"], "+1555");
    ABP_CHECK_EQ(rows[0]["body"], "Hello, world, date=not a column"); // Last column keeps everything.
    ABP_CHECK_EQ(rows[1]["address"], "");                              // NULL reads as empty.
    ABP_CHECK_EQ(rows[1]["body"], "multi\nline");
}

ABP_TEST(personal_parse_does_not_split_a_body_on_an_unrelated_row_marker) {
    const std::string output =
        "Row: 0 _id=1, body=first\nRow: 5 looks like a row but is text\n"
        "Row: 1 _id=2, body=second\n";
    auto rows = parseContentQuery(output, {"_id", "body"});
    ABP_CHECK_EQ(rows.size(), 2u);
    ABP_CHECK_EQ(rows[0]["body"], "first\nRow: 5 looks like a row but is text");
    ABP_CHECK_EQ(rows[1]["body"], "second");
}

ABP_TEST(personal_parse_handles_no_results_and_unexpected_columns) {
    ABP_CHECK_EQ(parseContentQuery("No result found.\n", {"_id"}).size(), 0u);
    ABP_CHECK_EQ(parseContentQuery("", {"_id"}).size(), 0u);
    ABP_CHECK_EQ(parseContentQuery("Row: 0 other=1\n", {"_id"}).size(), 0u);
}

ABP_TEST(personal_encodes_uri_components_like_android) {
    ABP_CHECK_EQ(encodeUriComponent("0r1-ABC:0r2.x_y~"), "0r1-ABC%3A0r2.x_y~");
    ABP_CHECK_EQ(encodeUriComponent("a b/%"), "a%20b%2F%25");
}

ABP_TEST(personal_builds_vcards_from_contact_data_rows) {
    std::vector<Row> rows = {
        {{"contact_id", "1"}, {"mimetype", "vnd.android.cursor.item/name"}, {"data1", "Ada Lovelace"},
         {"data2", "Ada"}, {"data3", "Lovelace"}},
        {{"contact_id", "1"}, {"mimetype", "vnd.android.cursor.item/phone_v2"}, {"data1", "+44 20 1234"},
         {"data2", "2"}},
        {{"contact_id", "1"}, {"mimetype", "vnd.android.cursor.item/note"}, {"data1", "likes; commas, too\nand lines"}},
        {{"contact_id", "2"}, {"mimetype", "vnd.android.cursor.item/email_v2"}, {"data1", "bob@example.com"}},
        {{"contact_id", "3"}, {"mimetype", "vnd.android.cursor.item/photo"}, {"data1", ""}},
    };
    int count = 0;
    const std::string vcf = buildVcards(rows, &count);
    ABP_CHECK_EQ(count, 2); // Contact 3 has nothing identifying.
    ABP_CHECK(vcf.find("FN:Ada Lovelace\r\n") != std::string::npos);
    ABP_CHECK(vcf.find("N:Lovelace;Ada;;;\r\n") != std::string::npos);
    ABP_CHECK(vcf.find("TEL;TYPE=CELL:+44 20 1234\r\n") != std::string::npos);
    ABP_CHECK(vcf.find("NOTE:likes\\; commas\\, too\\nand lines\r\n") != std::string::npos);
    ABP_CHECK(vcf.find("FN:bob@example.com\r\n") != std::string::npos); // Falls back to the email.
}

const ExportResult& resultFor(const std::vector<ExportResult>& results, const std::string& kind) {
    for (const auto& result : results) {
        if (result.kind == kind) return result;
    }
    throw std::runtime_error("no export result for " + kind);
}

const PersonalDataExport* exportFor(const Manifest& manifest, const std::string& kind) {
    for (const auto& item : manifest.personalDataExports) {
        if (item.kind == kind) return &item;
    }
    return nullptr;
}

DeviceInfo unrootedDevice() {
    DeviceInfo device;
    device.sdkInt = 34;
    return device;
}

ABP_TEST(personal_exports_contacts_sms_and_skips_a_refused_provider) {
    PersonalFixture fixture(kPhoneScript);
    Manifest manifest;

    auto results = exportPersonalData(AdbClient("SERIAL"), fixture.backupDir(), unrootedDevice(), manifest);
    ABP_CHECK_EQ(resultFor(results, "contacts").count, 2);
    ABP_CHECK_EQ(resultFor(results, "sms").count, 3);
    ABP_CHECK_EQ(resultFor(results, "call_log").count, -1);
    ABP_CHECK(resultFor(results, "call_log").reason.find("Permission Denial") != std::string::npos);
    ABP_CHECK_EQ(resultFor(results, "wifi").count, -1); // Needs root.
    ABP_CHECK(resultFor(results, "wifi").reason.find("root") != std::string::npos);

    const PersonalDataExport* contacts = exportFor(manifest, "contacts");
    ABP_CHECK(contacts != nullptr);
    ABP_CHECK_EQ(contacts->localPath, "personal/contacts.vcf");
    ABP_CHECK_EQ(contacts->sha256.size(), 64u);
    ABP_CHECK(exportFor(manifest, "sms") != nullptr);
    ABP_CHECK(exportFor(manifest, "call_log") == nullptr);

    const std::string vcf = fsutil::readTextFile(fixture.backupDir() / "personal" / "contacts.vcf");
    ABP_CHECK(vcf.find("FN:Ada") != std::string::npos);
    ABP_CHECK(vcf.find("FN:Bob") != std::string::npos);
    ABP_CHECK(!fs::exists(fixture.backupDir() / "personal" / "call_log.json"));

    json::JsonValue sms = json::JsonValue::parse(fsutil::readTextFile(fixture.backupDir() / "personal" / "sms.json"));
    const json::JsonValue list = sms.get("messages");
    const auto& messages = list.items();
    ABP_CHECK_EQ(messages.size(), 3u);
    ABP_CHECK_EQ(messages[0].get("body").asString(), "Hi, see you at 5, ok?");
    ABP_CHECK_EQ(messages[0].get("type").asString(), "inbox");
    ABP_CHECK_EQ(messages[0].get("date_utc").asString(), "2023-11-14T22:13:20Z");
    ABP_CHECK_EQ(messages[1].get("body").asString(), "Line one\nRow: 7 is just text in the same message");
    ABP_CHECK_EQ(messages[1].get("type").asString(), "sent");
    ABP_CHECK_EQ(messages[2].get("address").asString(), "");
    ABP_CHECK_EQ(messages[2].get("type").asString(), "draft");
}

ABP_TEST(personal_falls_back_to_building_vcards_when_content_read_is_missing) {
    PersonalFixture fixture(R"SH(
[ "$1" = "shell" ] || exit 1
case "$2" in
  *"content://com.android.contacts/contacts'"*)
    echo "Row: 0 _id=1, lookup=key1"; exit 0;;
  *"read --uri"*)
    echo "usage: adb shell content [subcommand] [options]"; exit 1;;
  *"content://com.android.contacts/data'"*)
    echo "Row: 0 contact_id=1, mimetype=vnd.android.cursor.item/name, data2=Grace, data3=Hopper, data1=Grace Hopper"
    echo "Row: 1 contact_id=1, mimetype=vnd.android.cursor.item/phone_v2, data2=1, data3=NULL, data1=555-0100"
    exit 0;;
esac
exit 1
)SH");
    Manifest manifest;
    auto results = exportPersonalData(AdbClient("SERIAL"), fixture.backupDir(), unrootedDevice(), manifest);
    ABP_CHECK_EQ(resultFor(results, "contacts").count, 1);
    const std::string vcf = fsutil::readTextFile(fixture.backupDir() / "personal" / "contacts.vcf");
    ABP_CHECK(vcf.find("FN:Grace Hopper\r\n") != std::string::npos);
    ABP_CHECK(vcf.find("TEL;TYPE=HOME:555-0100\r\n") != std::string::npos);
}

ABP_TEST(personal_builds_an_icalendar_document) {
    std::vector<Row> events = {
        {{"_id", "7"}, {"calendar_id", "1"}, {"dtstart", "1700000000000"}, {"dtend", "1700003600000"},
         {"allDay", "0"}, {"rrule", "FREQ=WEEKLY;BYDAY=TU"}, {"title", "Stand-up, weekly"},
         {"eventLocation", "Room 1"}, {"description", std::string(100, 'x')}},
        {{"_id", "8"}, {"calendar_id", "2"}, {"dtstart", "1700006400000"}, {"dtend", "0"}, {"allDay", "1"},
         {"title", "Holiday"}},
        {{"_id", "9"}, {"calendar_id", "1"}, {"dtstart", "0"}, {"title", "no start, skipped"}},
    };
    int count = 0;
    const std::string ics = buildIcs(events, {{"1", "Work"}, {"2", "Personal"}}, &count);
    ABP_CHECK_EQ(count, 2);
    ABP_CHECK(ics.find("BEGIN:VCALENDAR\r\n") == 0);
    ABP_CHECK(ics.find("DTSTART:20231114T221320Z\r\n") != std::string::npos);
    ABP_CHECK(ics.find("DTEND:20231114T231320Z\r\n") != std::string::npos);
    ABP_CHECK(ics.find("RRULE:FREQ=WEEKLY;BYDAY=TU\r\n") != std::string::npos);
    ABP_CHECK(ics.find("SUMMARY:Stand-up\\, weekly\r\n") != std::string::npos);
    ABP_CHECK(ics.find("CATEGORIES:Work\r\n") != std::string::npos);
    ABP_CHECK(ics.find("DTSTART;VALUE=DATE:20231115\r\n") != std::string::npos);
    ABP_CHECK(ics.find("DTEND;VALUE=DATE:20231116\r\n") != std::string::npos); // One day when no end is stored.
    // Long lines are folded at 75 octets.
    for (const auto& line : strutil::split(ics, '\n')) ABP_CHECK(line.size() <= 76); // 75 + '\r'
    ABP_CHECK(ics.find("\r\n x") != std::string::npos);
    ABP_CHECK(ics.find("END:VCALENDAR\r\n") != std::string::npos);
}

ABP_TEST(personal_parses_settings_listings) {
    auto values = parseSettingsList("screen_brightness=102\nnext_alarm_formatted=\nweird=a=b\r\nno equals\n");
    ABP_CHECK_EQ(values.size(), 3u);
    ABP_CHECK_EQ(values["screen_brightness"], "102");
    ABP_CHECK_EQ(values["next_alarm_formatted"], "");
    ABP_CHECK_EQ(values["weird"], "a=b");
}

ABP_TEST(personal_parses_wifi_config_store) {
    const std::string xml = R"XML(<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<WifiConfigStoreData>
<NetworkList>
<Network>
<WifiConfiguration>
<string name="ConfigKey">&quot;Home &amp; Garden&quot;WPA_PSK</string>
<string name="SSID">&quot;Home &amp; Garden&quot;</string>
<string name="PreSharedKey">&quot;s3cret&quot;</string>
<boolean name="HiddenSSID" value="true" />
</WifiConfiguration>
</Network>
<Network>
<WifiConfiguration>
<string name="ConfigKey">&quot;Cafe&quot;NONE</string>
<string name="SSID">&quot;Cafe&quot;</string>
</WifiConfiguration>
</Network>
<Network>
<WifiConfiguration>
<string name="ConfigKey">&quot;Office&quot;WPA_EAP</string>
<string name="SSID">&quot;Office&quot;</string>
</WifiConfiguration>
</Network>
</NetworkList>
</WifiConfigStoreData>)XML";
    auto networks = parseWifiConfigStore(xml);
    ABP_CHECK_EQ(networks.size(), 3u);
    ABP_CHECK_EQ(networks[0].ssid, "Home & Garden");
    ABP_CHECK_EQ(networks[0].password, "s3cret");
    ABP_CHECK_EQ(networks[0].security, "wpa2");
    ABP_CHECK(networks[0].hidden);
    ABP_CHECK_EQ(networks[1].security, "open");
    ABP_CHECK_EQ(networks[2].security, "WPA_EAP");

    ABP_CHECK_EQ(wifiAddNetworkCommand(networks[0]), "cmd wifi add-network 'Home & Garden' wpa2 's3cret' -h");
    ABP_CHECK_EQ(wifiAddNetworkCommand(networks[1]), "cmd wifi add-network 'Cafe' open");
    ABP_CHECK_EQ(wifiAddNetworkCommand(networks[2]), ""); // Enterprise cannot be re-created this way.
    WifiNetwork noPassword = networks[0];
    noPassword.password.clear();
    ABP_CHECK_EQ(wifiAddNetworkCommand(noPassword), "");
}

ABP_TEST(personal_parses_wpa_supplicant_conf) {
    auto networks = parseWpaSupplicant(
        "ctrl_interface=wlan0\nnetwork={\n\tssid=\"Old\"\n\tpsk=\"pass phrase\"\n\tkey_mgmt=WPA-PSK\n}\n"
        "network={\n\tssid=\"Open\"\n\tkey_mgmt=NONE\n\tscan_ssid=1\n}\n");
    ABP_CHECK_EQ(networks.size(), 2u);
    ABP_CHECK_EQ(networks[0].ssid, "Old");
    ABP_CHECK_EQ(networks[0].password, "pass phrase");
    ABP_CHECK_EQ(networks[0].security, "wpa2");
    ABP_CHECK_EQ(networks[1].security, "open");
    ABP_CHECK(networks[1].hidden);
}

ABP_TEST(personal_exports_mms_calendar_settings_and_rooted_wifi) {
    PersonalFixture fixture(R"SH(
if [ "$1" = "exec-out" ]; then
  case "$2" in
    *"content://mms/part/21'"*) printf 'JPEGDATA'; exit 0;;
  esac
  exit 1
fi
[ "$1" = "shell" ] || exit 1
case "$2" in
  *"content://mms'"*)
    echo "Row: 0 _id=5, thread_id=3, date=1700000000, msg_box=1, sub=NULL"; exit 0;;
  *"content://mms/part'"*)
    echo "Row: 0 _id=20, mid=5, seq=0, ct=text/plain, name=NULL, text=Look, a cat"
    echo "Row: 1 _id=21, mid=5, seq=1, ct=image/jpeg, name=cat.jpg, text=NULL"
    echo "Row: 2 _id=22, mid=5, seq=-1, ct=application/smil, name=smil.xml, text=<smil/>"
    exit 0;;
  *"conversations?simple=true"*)
    echo "Row: 0 _id=3, recipient_ids=1 2"; exit 0;;
  *"canonical-addresses"*)
    echo "Row: 0 _id=1, address=+15550001"
    echo "Row: 1 _id=2, address=+15550002"
    exit 0;;
  *"content://com.android.calendar/calendars'"*)
    echo "Row: 0 _id=1, calendar_displayName=Phone"; exit 0;;
  *"content://com.android.calendar/events'"*"--where 'deleted=0'"*)
    echo "Row: 0 _id=4, calendar_id=1, dtstart=1700000000000, dtend=1700003600000, allDay=0, rrule=NULL, duration=NULL, eventTimezone=UTC, eventLocation=NULL, title=Dentist, description=NULL"
    exit 0;;
  "settings list system") echo "screen_brightness=90"; exit 0;;
  "settings list secure") echo "location_mode=3"; exit 0;;
  "settings list global") echo "airplane_mode_on=0"; exit 0;;
  *"cat '/data/misc/apexdata/com.android.wifi/WifiConfigStore.xml'"*)
    printf '<Network>\n<string name="ConfigKey">&quot;Home&quot;WPA_PSK</string>\n<string name="SSID">&quot;Home&quot;</string>\n<string name="PreSharedKey">&quot;pw&quot;</string>\n</Network>\n'
    exit 0;;
esac
exit 1
)SH");
    DeviceInfo rooted = unrootedDevice();
    rooted.root.method = RootMethod::AdbdRoot;
    Manifest manifest;
    auto results = exportPersonalData(AdbClient("SERIAL"), fixture.backupDir(), rooted, manifest);

    ABP_CHECK_EQ(resultFor(results, "mms").count, 1);
    json::JsonValue mms = json::JsonValue::parse(fsutil::readTextFile(fixture.backupDir() / "personal" / "mms.json"));
    const json::JsonValue mmsMessages = mms.get("messages");
    const json::JsonValue& message = mmsMessages.items().at(0);
    ABP_CHECK_EQ(message.get("date").asInt(), 1700000000000LL); // Seconds converted to milliseconds.
    ABP_CHECK_EQ(message.get("box").asString(), "inbox");
    const json::JsonValue people = message.get("participants");
    ABP_CHECK_EQ(people.items().size(), 2u);
    const json::JsonValue parts = message.get("parts");
    ABP_CHECK_EQ(parts.items().size(), 2u); // SMIL layout dropped.
    ABP_CHECK_EQ(parts.items()[0].get("text").asString(), "Look, a cat");
    ABP_CHECK_EQ(parts.items()[1].get("file").asString(), "mms_parts/21.jpg");
    ABP_CHECK_EQ(fsutil::readTextFile(fixture.backupDir() / "personal" / "mms_parts" / "21.jpg"), "JPEGDATA");

    ABP_CHECK_EQ(resultFor(results, "calendar").count, 1);
    const std::string ics = fsutil::readTextFile(fixture.backupDir() / "personal" / "calendar.ics");
    ABP_CHECK(ics.find("SUMMARY:Dentist") != std::string::npos);
    ABP_CHECK(ics.find("CATEGORIES:Phone") != std::string::npos);

    ABP_CHECK_EQ(resultFor(results, "settings").count, 3);

    ABP_CHECK_EQ(resultFor(results, "wifi").count, 1);
    const fs::path wifi = fixture.backupDir() / "personal" / "wifi.json";
    ABP_CHECK(fsutil::readTextFile(wifi).find("\"pw\"") != std::string::npos);
    ABP_CHECK((fs::status(wifi).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
}

ABP_TEST(personal_restore_copies_imports_and_re_adds_wifi) {
    PersonalFixture fixture(R"SH(
[ "$1" = "push" ] && exit 0
case "$2" in
  "cmd wifi add-network"*) exit 0;;
esac
exit 1
)SH");
    fsutil::writeTextFile(fixture.backupDir() / "personal" / "contacts.vcf", "BEGIN:VCARD\r\nEND:VCARD\r\n");
    fsutil::writeTextFile(fixture.backupDir() / "personal" / "calendar.ics", "BEGIN:VCALENDAR\r\nEND:VCALENDAR\r\n");
    fsutil::writeTextFile(fixture.backupDir() / "personal" / "wifi.json",
                          R"({"networks": [{"ssid": "Home", "security": "wpa2", "password": "pw", "hidden": false},
                                           {"ssid": "Corp", "security": "WPA_EAP", "password": "", "hidden": false}]})");
    Manifest manifest;
    for (const auto& [kind, file] : std::vector<std::pair<std::string, std::string>>{
             {"contacts", "contacts.vcf"}, {"calendar", "calendar.ics"}, {"wifi", "wifi.json"}}) {
        PersonalDataExport item;
        item.kind = kind;
        item.localPath = "personal/" + file;
        item.itemCount = kind == "wifi" ? 2 : 1;
        manifest.personalDataExports.push_back(item);
    }

    RestoreResult result = restorePersonalData(AdbClient("SERIAL"), fixture.backupDir(), manifest, 34);
    ABP_CHECK_EQ(result.contactsPath, "/sdcard/Download/abp-contacts.vcf");
    ABP_CHECK_EQ(result.calendarPath, "/sdcard/Download/abp-calendar.ics");
    ABP_CHECK_EQ(result.wifiRestored, 1);
    ABP_CHECK_EQ(result.wifiSkipped, 1);
    ABP_CHECK(fixture.readLog().find("add-network 'Home' wpa2 'pw'") != std::string::npos);

    // Too old for `cmd wifi add-network`: nothing attempted.
    RestoreResult old = restorePersonalData(AdbClient("SERIAL"), fixture.backupDir(), manifest, 29);
    ABP_CHECK_EQ(old.wifiRestored, -1);

    // A tampered export is not copied.
    manifest.personalDataExports[0].sha256 = std::string(64, '0');
    ABP_CHECK_EQ(restorePersonalData(AdbClient("SERIAL"), fixture.backupDir(), manifest, 34).contactsPath, "");
}

ABP_TEST(personal_manifest_round_trips_exports_and_de_archives) {
    Manifest manifest;
    PersonalDataExport item;
    item.kind = "sms";
    item.format = "json";
    item.localPath = "personal/sms.json";
    item.itemCount = 42;
    item.bytes = 1234;
    item.sha256 = "abc";
    manifest.personalDataExports.push_back(item);
    PackageBackupEntry entry;
    entry.name = "com.android.providers.telephony";
    entry.deDataArchive = "data/com.android.providers.telephony.de.tar.gz";
    entry.deDataArchiveBytes = 99;
    entry.deDataArchiveSha256 = "def";
    manifest.packages.push_back(entry);

    Manifest parsed = Manifest::fromJson(manifest.toJson());
    ABP_CHECK_EQ(parsed.personalDataExports.size(), 1u);
    ABP_CHECK_EQ(parsed.personalDataExports[0].itemCount, 42);
    ABP_CHECK_EQ(parsed.personalDataExports[0].localPath, "personal/sms.json");
    ABP_CHECK_EQ(parsed.packages[0].deDataArchive, "data/com.android.providers.telephony.de.tar.gz");
    ABP_CHECK_EQ(parsed.packages[0].deDataArchiveBytes, 99ULL);
    ABP_CHECK_EQ(parsed.packages[0].deDataArchiveSha256, "def");
}
