#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "FakeAdb.h"
#include "TestFramework.h"
#include "abp/AdbClient.h"
#include "abp/BackupManager.h"
#include "abp/FsUtil.h"
#include "abp/Logger.h"
#include "abp/Manifest.h"
#include "abp/Process.h"
#include "abp/RootBackend.h"
#include "abp/StandardBackend.h"

using namespace abp;
using abp::test::FakeAdb;
namespace fs = std::filesystem;

namespace {

/// A freshly reset, unrooted phone: one ready device, no third-party apps,
/// and a little shared storage.
const char* kEmptyPhoneScript = R"SH(
[ "$1" = "-s" ] && shift 2
case "$1" in
  version) exit 0;;
  devices)
    echo "List of devices attached"
    echo "FRESH                  device model:Phone"
    exit 0;;
  shell)
    case "$2" in
      "id -u") echo 2000; exit 0;;
      getprop*) echo 14; exit 0;;
      "pm list packages"*) exit 0;;
    esac
    exit 1;;
  pull)
    shift
    [ "$1" = "-a" ] && shift
    mkdir -p "$2/DCIM" && echo "jpeg" > "$2/DCIM/photo.jpg"
    exit 0;;
esac
exit 1
)SH";

} // namespace

ABP_TEST(backup_of_a_phone_without_apps_still_captures_shared_storage) {
    FakeAdb fixture(kEmptyPhoneScript);

    BackupOptions options;
    options.outputDir = fixture.backupDir();
    options.assumeYes = true;

    BackupSummary summary = BackupManager::runBackup(options);
    ABP_CHECK(summary.success);
    ABP_CHECK(summary.sharedStorageIncluded);
    ABP_CHECK_EQ(summary.packageCount, 0);

    Manifest manifest = Manifest::readFromFile(fixture.backupDir() / "manifest.json");
    ABP_CHECK(manifest.sharedStorageIncluded);
    ABP_CHECK(fs::exists(fixture.backupDir() / "shared_storage" / "DCIM" / "photo.jpg"));
}

ABP_TEST(backup_still_fails_when_named_packages_match_nothing) {
    FakeAdb fixture(kEmptyPhoneScript);

    BackupOptions options;
    options.outputDir = fixture.backupDir();
    options.onlyPackages = {"com.example.typo"};
    options.assumeYes = true;

    BackupSummary summary = BackupManager::runBackup(options);
    ABP_CHECK(!summary.success);
    ABP_CHECK(!summary.messages.empty());
}

ABP_TEST(backup_reports_a_connection_problem_instead_of_running) {
    FakeAdb fixture(R"SH(
case "$1" in
  devices)
    echo "List of devices attached"
    echo "LOCKED                 unauthorized"
    exit 0;;
esac
exit 1
)SH");

    BackupOptions options;
    options.outputDir = fixture.backupDir();
    BackupSummary summary = BackupManager::runBackup(options);
    ABP_CHECK(!summary.success);
    ABP_CHECK_EQ(summary.messages.size(), 1u);
    ABP_CHECK(summary.messages[0].find("unauthorized") != std::string::npos);
    ABP_CHECK(fixture.readLog().find("shell") == std::string::npos); // Never got as far as the device.
}

ABP_TEST(standard_keeps_a_partially_pulled_shared_storage) {
    // adb stops at the first unreadable file and exits non-zero, but what it
    // copied before that is real data that belongs in the backup.
    FakeAdb fixture(R"SH(
[ "$1" = "-s" ] && shift 2
if [ "$1" = "pull" ]; then
  shift
  [ "$1" = "-a" ] && shift
  mkdir -p "$2/Music" && echo "song" > "$2/Music/a.mp3"
  echo "adb: error: failed to copy '/sdcard/Android/data/x' to '$2/Android/data/x': Permission denied" >&2
  exit 1
fi
exit 1
)SH");

    Manifest manifest;
    manifest.mode = "standard";
    StandardBackend backend;
    ABP_CHECK(backend.backupSharedStorage(AdbClient("SERIAL"), fixture.backupDir(), manifest));
    ABP_CHECK(manifest.sharedStorageIncluded);
    ABP_CHECK(manifest.sharedStorageIsDirectory);
    ABP_CHECK(manifest.sharedStorageArchiveBytes > 0);
}

ABP_TEST(root_restore_skips_an_app_that_is_not_installed) {
    // No /data/data/<pkg> to stat: extracting anyway would create a root-owned
    // data directory that the app, once installed, cannot use.
    FakeAdb fixture(R"SH(
[ "$1" = "-s" ] && shift 2
case "$2" in
  *"am force-stop"*) exit 0;;
  *"stat -c"*) exit 1;;
esac
exit 1
)SH");

    fsutil::ensureDirectory(fixture.backupDir() / "data");
    fsutil::writeTextFile(fixture.backupDir() / "data" / "com.example.app.tar.gz", "payload");

    Manifest manifest;
    manifest.mode = "root";
    PackageBackupEntry entry;
    entry.name = "com.example.app";
    entry.dataIncluded = true;
    entry.dataCaptureMethod = DataCaptureMethod::RootTar;
    entry.dataArchive = "data/com.example.app.tar.gz";
    manifest.packages.push_back(entry);

    RootAccess root;
    root.method = RootMethod::AdbdRoot;
    RootBackend backend(root);
    backend.restoreAppData(AdbClient("SERIAL"), fixture.backupDir(), manifest, {"com.example.app"});

    ABP_CHECK(manifest.packages[0].error.find("not installed") != std::string::npos);
    ABP_CHECK(fixture.readLog().find("tar -xzf") == std::string::npos);
}

ABP_TEST(root_restore_extracts_and_fixes_ownership_for_an_installed_app) {
    FakeAdb fixture(R"SH(
[ "$1" = "-s" ] && shift 2
case "$2" in
  *"am force-stop"*) exit 0;;
  *"stat -c"*) echo "10123:10123"; exit 0;;
  *"tar -xzf"*) cat > /dev/null; exit 0;;
  *"chown -R"*|*"restorecon"*) exit 0;;
esac
exit 1
)SH");

    fsutil::ensureDirectory(fixture.backupDir() / "data");
    fsutil::writeTextFile(fixture.backupDir() / "data" / "com.example.app.tar.gz", "payload");

    Manifest manifest;
    manifest.mode = "root";
    PackageBackupEntry entry;
    entry.name = "com.example.app";
    entry.dataIncluded = true;
    entry.dataCaptureMethod = DataCaptureMethod::RootTar;
    entry.dataArchive = "data/com.example.app.tar.gz";
    manifest.packages.push_back(entry);

    RootAccess root;
    root.method = RootMethod::AdbdRoot;
    RootBackend backend(root);
    backend.restoreAppData(AdbClient("SERIAL"), fixture.backupDir(), manifest, {"com.example.app"});

    ABP_CHECK_EQ(manifest.packages[0].error, std::string());
    const std::string log = fixture.readLog();
    ABP_CHECK(log.find("tar -xzf") != std::string::npos);
    ABP_CHECK(log.find("chown -R '10123:10123' '/data/data/com.example.app'") != std::string::npos);
}

ABP_TEST(root_backup_captures_device_protected_data_too) {
    // The SMS database lives in /data/user_de/0/com.android.providers.telephony,
    // not in /data/data, so root mode must tar both.
    FakeAdb fixture(R"SH(
[ "$1" = "-s" ] && shift 2
case "$2" in
  *"[ -d '/data/data/com.android.providers.telephony' ]"*) echo yes; exit 0;;
  *"[ -d '/data/user_de/0/com.android.providers.telephony' ]"*) echo yes; exit 0;;
  *"-C '/data/data' 'com.android.providers.telephony'"*) printf 'CE_ARCHIVE'; exit 0;;
  *"-C '/data/user_de/0' 'com.android.providers.telephony'"*) printf 'DE_ARCHIVE_WITH_SMS'; exit 0;;
esac
exit 1
)SH");

    PackageInfo pkg;
    pkg.name = "com.android.providers.telephony";
    Manifest manifest;
    PackageBackupEntry entry;
    entry.name = pkg.name;
    manifest.packages.push_back(entry);

    RootAccess root;
    root.method = RootMethod::AdbdRoot;
    RootBackend(root).backupAppData(AdbClient("SERIAL"), fixture.backupDir(), {pkg}, manifest);

    const PackageBackupEntry& captured = manifest.packages[0];
    ABP_CHECK(captured.dataIncluded);
    ABP_CHECK_EQ(captured.error, std::string());
    ABP_CHECK_EQ(captured.dataArchive, "data/com.android.providers.telephony.tar.gz");
    ABP_CHECK_EQ(captured.deDataArchive, "data/com.android.providers.telephony.de.tar.gz");
    ABP_CHECK_EQ(captured.deDataArchiveSha256.size(), 64u);
    ABP_CHECK_EQ(fsutil::readTextFile(fixture.backupDir() / captured.deDataArchive), "DE_ARCHIVE_WITH_SMS");
}

ABP_TEST(root_restore_writes_back_device_protected_data_with_its_own_owner) {
    FakeAdb fixture(R"SH(
[ "$1" = "-s" ] && shift 2
case "$2" in
  *"am force-stop"*) exit 0;;
  *"stat -c '%u:%g' '/data/data/"*) echo "1001:1001"; exit 0;;
  *"stat -c '%u:%g' '/data/user_de/0/"*) echo "1001:1002"; exit 0;;
  *"tar -xzf"*) cat > /dev/null; exit 0;;
  *"chown -R"*|*"restorecon"*) exit 0;;
esac
exit 1
)SH");

    fsutil::ensureDirectory(fixture.backupDir() / "data");
    fsutil::writeTextFile(fixture.backupDir() / "data" / "p.tar.gz", "ce");
    fsutil::writeTextFile(fixture.backupDir() / "data" / "p.de.tar.gz", "de");

    Manifest manifest;
    manifest.mode = "root";
    PackageBackupEntry entry;
    entry.name = "com.android.providers.telephony";
    entry.isSystemApp = true;
    entry.dataIncluded = true;
    entry.dataCaptureMethod = DataCaptureMethod::RootTar;
    entry.dataArchive = "data/p.tar.gz";
    entry.deDataArchive = "data/p.de.tar.gz";
    manifest.packages.push_back(entry);

    RootAccess root;
    root.method = RootMethod::AdbdRoot;
    RootBackend(root).restoreAppData(AdbClient("SERIAL"), fixture.backupDir(), manifest, {entry.name});

    ABP_CHECK_EQ(manifest.packages[0].error, std::string());
    const std::string log = fixture.readLog();
    ABP_CHECK(log.find("tar -xzf - -C '/data/user_de/0'") != std::string::npos);
    ABP_CHECK(log.find("chown -R '1001:1002' '/data/user_de/0/com.android.providers.telephony'") != std::string::npos);
    ABP_CHECK(log.find("chown -R '1001:1001' '/data/data/com.android.providers.telephony'") != std::string::npos);
}

ABP_TEST(root_restore_refuses_a_corrupted_device_protected_archive) {
    FakeAdb fixture("exit 0\n");
    fsutil::ensureDirectory(fixture.backupDir() / "data");
    fsutil::writeTextFile(fixture.backupDir() / "data" / "p.tar.gz", "ce");
    fsutil::writeTextFile(fixture.backupDir() / "data" / "p.de.tar.gz", "tampered");

    Manifest manifest;
    PackageBackupEntry entry;
    entry.name = "com.example.app";
    entry.dataIncluded = true;
    entry.dataArchive = "data/p.tar.gz";
    entry.deDataArchive = "data/p.de.tar.gz";
    entry.deDataArchiveSha256 = std::string(64, 'a');
    manifest.packages.push_back(entry);

    RootAccess root;
    root.method = RootMethod::AdbdRoot;
    RootBackend(root).restoreAppData(AdbClient("SERIAL"), fixture.backupDir(), manifest, {entry.name});

    ABP_CHECK(manifest.packages[0].error.find("checksum mismatch") != std::string::npos);
    ABP_CHECK(fixture.readLog().find("tar -xzf") == std::string::npos); // Neither half touched.
}

namespace {

/// An unrooted Android 14 phone with two apps -- neither debuggable -- one of
/// them an authenticator, and an SD card.
const char* kTwoAppPhoneScript = R"SH(
[ "$1" = "-s" ] && shift 2
case "$1" in
  version) exit 0;;
  devices)
    echo "List of devices attached"
    echo "PHONE                  device model:Phone"
    exit 0;;
  shell)
    case "$2" in
      "id -u") echo 2000; exit 0;;
      "getprop ro.build.version.sdk") echo 34; exit 0;;
      getprop*) echo x; exit 0;;
      "pm list packages"*)
        echo "package:/data/app/a/base.apk=com.google.android.apps.authenticator2"
        echo "package:/data/app/b/base.apk=com.example.notes"
        exit 0;;
      "sm list-volumes public"*) echo "public:179,1 mounted 1A2B-3C4D"; exit 0;;
      *"[ -e '/storage/1A2B-3C4D' ]"*) echo yes; exit 0;;
    esac
    exit 1;;
  pull)
    shift
    [ "$1" = "-a" ] && shift
    mkdir -p "$2/DCIM" && echo "jpeg" > "$2/DCIM/photo.jpg"
    exit 0;;
esac
exit 1
)SH";

} // namespace

ABP_TEST(backup_copies_sd_cards_and_warns_about_what_it_could_not_capture) {
    FakeAdb fixture(kTwoAppPhoneScript);

    BackupOptions options;
    options.outputDir = fixture.backupDir();
    options.includeApks = false;
    options.assumeYes = true;

    BackupSummary summary = BackupManager::runBackup(options);
    ABP_CHECK(summary.success);

    // The SD card lands with the raw path captures.
    ABP_CHECK_EQ(summary.removableStorageCount, 1);
    Manifest manifest = Manifest::readFromFile(fixture.backupDir() / "manifest.json");
    ABP_CHECK_EQ(manifest.filesystemCaptures.size(), 1u);
    ABP_CHECK_EQ(manifest.filesystemCaptures[0].devicePath, "/storage/1A2B-3C4D");
    ABP_CHECK(fs::exists(fixture.backupDir() / manifest.filesystemCaptures[0].localPath / "DCIM" / "photo.jpg"));

    // Neither app is debuggable and `adb backup` produced nothing, so both
    // lack data -- and the authenticator is called out by name.
    ABP_CHECK_EQ(summary.packagesWithoutData, 2);
    bool mentionsAuthenticator = false;
    bool mentionsMissingData = false;
    for (const auto& warning : summary.warnings) {
        if (warning.find("com.google.android.apps.authenticator2") != std::string::npos) mentionsAuthenticator = true;
        if (warning.find("could not be captured for 2 app(s)") != std::string::npos) mentionsMissingData = true;
    }
    ABP_CHECK(mentionsAuthenticator);
    ABP_CHECK(mentionsMissingData);
}

ABP_TEST(backup_leaves_sd_cards_alone_with_no_shared) {
    FakeAdb fixture(kTwoAppPhoneScript);

    BackupOptions options;
    options.outputDir = fixture.backupDir();
    options.includeApks = false;
    options.includeSharedStorage = false;
    options.exportPersonalData = false;

    BackupSummary summary = BackupManager::runBackup(options);
    ABP_CHECK(summary.success);
    ABP_CHECK_EQ(summary.removableStorageCount, 0);
    ABP_CHECK(fixture.readLog().find("list-volumes") == std::string::npos);
}

ABP_TEST(backup_reports_progress_per_package) {
    FakeAdb fixture(kTwoAppPhoneScript);
    std::vector<std::string> events;
    Logger::setProgressSink([&events](const std::string& step, int done, int total, const std::string& item) {
        events.push_back(step + " " + std::to_string(done) + "/" + std::to_string(total) + " " + item);
    });

    BackupOptions options;
    options.outputDir = fixture.backupDir();
    options.includeSharedStorage = false;
    options.exportPersonalData = false;
    BackupSummary summary = BackupManager::runBackup(options);
    Logger::setProgressSink(nullptr);

    ABP_CHECK(summary.success);
    auto has = [&events](const std::string& event) {
        return std::find(events.begin(), events.end(), event) != events.end();
    };
    ABP_CHECK(has("Extracting APKs 0/2 com.google.android.apps.authenticator2"));
    ABP_CHECK(has("Backing up app data 1/2 com.example.notes"));
    ABP_CHECK(has("Backing up app data 2/2 ")); // The step reports its completion.
}

ABP_TEST(backup_cancelled_mid_run_keeps_a_truthful_manifest) {
    FakeAdb fixture(kTwoAppPhoneScript);
    // Cancel as soon as the first package's APKs are being extracted.
    Logger::setProgressSink([](const std::string& step, int, int, const std::string&) {
        if (step == "Extracting APKs") Process::requestCancel();
    });

    BackupOptions options;
    options.outputDir = fixture.backupDir();
    BackupSummary summary = BackupManager::runBackup(options);
    Logger::setProgressSink(nullptr);
    Process::clearCancel();

    ABP_CHECK(!summary.success);
    ABP_CHECK(summary.cancelled);
    ABP_CHECK(summary.messages.at(0).find("cancelled") != std::string::npos);
    // Nothing after the cancel ran: no shared storage pull, no legacy backup.
    ABP_CHECK(fixture.readLog().find("pull -a /sdcard") == std::string::npos);
    ABP_CHECK(fixture.readLog().find("backup -f") == std::string::npos);

    // The manifest exists and says the packages were not captured, rather
    // than listing them as clean, empty successes.
    Manifest manifest = Manifest::readFromFile(fixture.backupDir() / "manifest.json");
    ABP_CHECK_EQ(manifest.packages.size(), 2u);
    for (const auto& entry : manifest.packages) ABP_CHECK(entry.error.find("cancelled") != std::string::npos);
}
