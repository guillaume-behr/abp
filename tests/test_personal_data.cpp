#include "abp/PersonalData.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "TestFramework.h"
#include "abp/FsUtil.h"
#include "abp/Json.h"

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

ABP_TEST(personal_exports_contacts_sms_and_skips_a_refused_provider) {
    PersonalFixture fixture(kPhoneScript);
    Manifest manifest;

    ExportCounts counts = exportPersonalData(AdbClient("SERIAL"), fixture.backupDir(), manifest);
    ABP_CHECK_EQ(counts.contacts, 2);
    ABP_CHECK_EQ(counts.sms, 3);
    ABP_CHECK_EQ(counts.callLog, -1);

    ABP_CHECK_EQ(manifest.personalDataExports.size(), 2u);
    ABP_CHECK_EQ(manifest.personalDataExports[0].kind, "contacts");
    ABP_CHECK_EQ(manifest.personalDataExports[0].localPath, "personal/contacts.vcf");
    ABP_CHECK_EQ(manifest.personalDataExports[0].sha256.size(), 64u);
    ABP_CHECK_EQ(manifest.personalDataExports[1].kind, "sms");

    const std::string vcf = fsutil::readTextFile(fixture.backupDir() / "personal" / "contacts.vcf");
    ABP_CHECK(vcf.find("FN:Ada") != std::string::npos);
    ABP_CHECK(vcf.find("FN:Bob") != std::string::npos);
    ABP_CHECK(!fs::exists(fixture.backupDir() / "personal" / "call_log.json"));

    json::JsonValue sms = json::JsonValue::parse(fsutil::readTextFile(fixture.backupDir() / "personal" / "sms.json"));
    const auto& messages = sms.at("messages").items();
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
    ExportCounts counts = exportPersonalData(AdbClient("SERIAL"), fixture.backupDir(), manifest);
    ABP_CHECK_EQ(counts.contacts, 1);
    const std::string vcf = fsutil::readTextFile(fixture.backupDir() / "personal" / "contacts.vcf");
    ABP_CHECK(vcf.find("FN:Grace Hopper\r\n") != std::string::npos);
    ABP_CHECK(vcf.find("TEL;TYPE=HOME:555-0100\r\n") != std::string::npos);
}

ABP_TEST(personal_pushes_contacts_to_download_for_import) {
    PersonalFixture fixture(R"SH(
[ "$1" = "push" ] && exit 0
exit 1
)SH");
    fsutil::writeTextFile(fixture.backupDir() / "personal" / "contacts.vcf", "BEGIN:VCARD\r\nEND:VCARD\r\n");
    Manifest manifest;
    PersonalDataExport item;
    item.kind = "contacts";
    item.localPath = "personal/contacts.vcf";
    item.itemCount = 1;
    manifest.personalDataExports.push_back(item);

    const std::string remote = pushContactsForImport(AdbClient("SERIAL"), fixture.backupDir(), manifest);
    ABP_CHECK_EQ(remote, "/sdcard/Download/abp-contacts.vcf");
    ABP_CHECK(fixture.readLog().find("push") != std::string::npos);

    // A tampered export is not copied.
    manifest.personalDataExports[0].sha256 = std::string(64, '0');
    ABP_CHECK_EQ(pushContactsForImport(AdbClient("SERIAL"), fixture.backupDir(), manifest), "");
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
