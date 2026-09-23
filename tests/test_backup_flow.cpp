#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "TestFramework.h"
#include "abp/AdbClient.h"
#include "abp/BackupManager.h"
#include "abp/FsUtil.h"
#include "abp/Manifest.h"
#include "abp/RootBackend.h"
#include "abp/StandardBackend.h"

using namespace abp;
namespace fs = std::filesystem;

namespace {

/// A fake `adb` plus a scratch directory, restored/removed on destruction.
/// Every invocation is appended to calls.log next to the script.
class FlowFixture {
public:
    explicit FlowFixture(const std::string& script) : previousPath_(AdbClient::adbPath()) {
        static int counter = 0;
        root_ = fs::temp_directory_path() /
                ("abp_flow_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        fs::create_directories(root_ / "backup");

        adbPath_ = root_ / "adb";
        std::ofstream out(adbPath_);
        out << "#!/bin/sh\necho \"$*\" >> \"$(dirname \"$0\")/calls.log\"\n" << script;
        out.close();
        fs::permissions(adbPath_, fs::perms::owner_all);
        AdbClient::setAdbPath(adbPath_.string());
    }

    ~FlowFixture() {
        AdbClient::setAdbPath(previousPath_);
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    FlowFixture(const FlowFixture&) = delete;
    FlowFixture& operator=(const FlowFixture&) = delete;

    fs::path backupDir() const { return root_ / "backup"; }

    std::string readLog() const {
        const fs::path log = root_ / "calls.log";
        return fs::exists(log) ? fsutil::readTextFile(log) : std::string();
    }

private:
    std::string previousPath_;
    fs::path root_;
    fs::path adbPath_;
};

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
    FlowFixture fixture(kEmptyPhoneScript);

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
    FlowFixture fixture(kEmptyPhoneScript);

    BackupOptions options;
    options.outputDir = fixture.backupDir();
    options.onlyPackages = {"com.example.typo"};
    options.assumeYes = true;

    BackupSummary summary = BackupManager::runBackup(options);
    ABP_CHECK(!summary.success);
    ABP_CHECK(!summary.messages.empty());
}

ABP_TEST(backup_reports_a_connection_problem_instead_of_running) {
    FlowFixture fixture(R"SH(
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
    FlowFixture fixture(R"SH(
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
    FlowFixture fixture(R"SH(
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
    FlowFixture fixture(R"SH(
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
    FlowFixture fixture(R"SH(
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
    FlowFixture fixture(R"SH(
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
    FlowFixture fixture("exit 0\n");
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
    FlowFixture fixture(kTwoAppPhoneScript);

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
    FlowFixture fixture(kTwoAppPhoneScript);

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
