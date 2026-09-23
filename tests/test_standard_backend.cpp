#include "abp/StandardBackend.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "FakeAdb.h"
#include "TestFramework.h"
#include "abp/AdbClient.h"
#include "abp/FsUtil.h"
#include "abp/Manifest.h"

using namespace abp;
using abp::test::FakeAdb;
namespace fs = std::filesystem;

namespace {

/// A device where `com.example.debuggable` is debuggable (so `run-as` works)
/// and `com.example.locked` is not. Records each call for later assertions.
std::string mixedDeviceScript() {
    return R"SH(
shift $(( $# - 1 ))
CMD="$1"
case "$CMD" in
  *"@@abp-runas:"*) echo "@@abp-runas:com.example.debuggable"; exit 0;;
  *"run-as 'com.example.debuggable' tar -czf -"*)
    printf 'FAKE_TAR_GZ_PAYLOAD_FOR_DEBUGGABLE'; exit 0;;
  *"run-as"*) exit 1;;
  *"am force-stop"*) exit 0;;
esac
exit 1
)SH";
}

std::vector<PackageInfo> twoPackages() {
    PackageInfo debuggable;
    debuggable.name = "com.example.debuggable";
    PackageInfo locked;
    locked.name = "com.example.locked";
    return {debuggable, locked};
}

Manifest manifestFor(const std::vector<PackageInfo>& packages) {
    Manifest manifest;
    manifest.mode = "standard";
    for (const auto& pkg : packages) {
        PackageBackupEntry entry;
        entry.name = pkg.name;
        manifest.packages.push_back(entry);
    }
    return manifest;
}

} // namespace

ABP_TEST(standard_captures_debuggable_packages_via_run_as) {
    FakeAdb fixture(mixedDeviceScript());
    auto packages = twoPackages();
    Manifest manifest = manifestFor(packages);

    StandardBackend backend;
    backend.backupAppData(AdbClient("SERIAL"), fixture.backupDir(), packages, manifest);

    const PackageBackupEntry* debuggable = findPackageEntry(manifest, "com.example.debuggable");
    ABP_CHECK(debuggable != nullptr);
    ABP_CHECK(debuggable->dataIncluded);
    ABP_CHECK(debuggable->dataCaptureMethod == DataCaptureMethod::RunAsTar);
    ABP_CHECK_EQ(debuggable->dataArchive, "data/com.example.debuggable.tar.gz");
    ABP_CHECK(debuggable->dataArchiveBytes > 0);
    ABP_CHECK_EQ(debuggable->dataArchiveSha256.size(), 64u);
    ABP_CHECK(fs::exists(fixture.backupDir() / "data" / "com.example.debuggable.tar.gz"));
}

ABP_TEST(standard_falls_back_to_legacy_only_for_non_debuggable_packages) {
    FakeAdb fixture(mixedDeviceScript());
    auto packages = twoPackages();
    Manifest manifest = manifestFor(packages);

    StandardBackend backend;
    backend.backupAppData(AdbClient("SERIAL"), fixture.backupDir(), packages, manifest);

    // The fake device's `adb backup` fails, so the non-debuggable package is
    // recorded as failed -- but the debuggable one is unaffected by that.
    const PackageBackupEntry* locked = findPackageEntry(manifest, "com.example.locked");
    ABP_CHECK(locked != nullptr);
    ABP_CHECK(!locked->dataIncluded);
    ABP_CHECK(!locked->error.empty());

    ABP_CHECK(findPackageEntry(manifest, "com.example.debuggable")->dataIncluded);

    // Crucially, the legacy backup must have been asked for only the package
    // that run-as could not reach.
    const std::string log = fixture.readLog();
    ABP_CHECK(log.find("com.example.locked") != std::string::npos);
    ABP_CHECK(log.find("backup") != std::string::npos);
    const size_t backupLine = log.find("backup -f");
    ABP_CHECK(backupLine != std::string::npos);
    const size_t lineEnd = log.find('\n', backupLine);
    const std::string backupArgs = log.substr(backupLine, lineEnd - backupLine);
    ABP_CHECK(backupArgs.find("com.example.debuggable") == std::string::npos);
}

ABP_TEST(standard_skips_the_legacy_flow_when_run_as_covers_everything) {
    FakeAdb fixture(mixedDeviceScript());

    PackageInfo only;
    only.name = "com.example.debuggable";
    std::vector<PackageInfo> packages{only};
    Manifest manifest = manifestFor(packages);

    StandardBackend backend;
    backend.backupAppData(AdbClient("SERIAL"), fixture.backupDir(), packages, manifest);

    // No legacy archive means no on-device confirmation prompt at all.
    ABP_CHECK(manifest.legacyAdbBackupFile.empty());
    ABP_CHECK(fixture.readLog().find("backup -f") == std::string::npos);
    ABP_CHECK(!fs::exists(fixture.backupDir() / "legacy_backup.ab"));
}

ABP_TEST(standard_restores_run_as_packages_individually) {
    FakeAdb fixture(R"SH(
shift $(( $# - 1 ))
case "$1" in
  *"run-as 'com.example.debuggable' tar -xzf -"*) cat >/dev/null; exit 0;;
  *"am force-stop"*) exit 0;;
esac
exit 1
)SH");

    // A backup containing one run-as package and one legacy package.
    Manifest manifest;
    manifest.mode = "standard";
    manifest.legacyAdbBackupFile = "legacy_backup.ab";

    PackageBackupEntry runAs;
    runAs.name = "com.example.debuggable";
    runAs.dataIncluded = true;
    runAs.dataCaptureMethod = DataCaptureMethod::RunAsTar;
    runAs.dataArchive = "data/com.example.debuggable.tar.gz";
    manifest.packages.push_back(runAs);

    PackageBackupEntry legacy;
    legacy.name = "com.example.locked";
    legacy.dataIncluded = true;
    legacy.dataCaptureMethod = DataCaptureMethod::LegacyAdbBackup;
    manifest.packages.push_back(legacy);

    fs::create_directories(fixture.backupDir() / "data");
    std::ofstream(fixture.backupDir() / "data" / "com.example.debuggable.tar.gz") << "payload";
    std::ofstream(fixture.backupDir() / "legacy_backup.ab") << "legacy";

    StandardBackend backend;
    // Selecting only the run-as package must not drag in the legacy archive,
    // which would restore the deselected package's data too.
    backend.restoreAppData(AdbClient("SERIAL"), fixture.backupDir(), manifest, {"com.example.debuggable"});

    const std::string log = fixture.readLog();
    ABP_CHECK(log.find("run-as 'com.example.debuggable' tar -xzf") != std::string::npos);
    ABP_CHECK(log.find("am force-stop") != std::string::npos);
    ABP_CHECK(log.find("restore") == std::string::npos);
    ABP_CHECK(findPackageEntry(manifest, "com.example.debuggable")->error.empty());
}

ABP_TEST(standard_restore_refuses_a_corrupted_run_as_archive) {
    FakeAdb fixture(R"SH(exit 0
)SH");

    Manifest manifest;
    manifest.mode = "standard";
    PackageBackupEntry entry;
    entry.name = "com.example.debuggable";
    entry.dataIncluded = true;
    entry.dataCaptureMethod = DataCaptureMethod::RunAsTar;
    entry.dataArchive = "data/com.example.debuggable.tar.gz";
    entry.dataArchiveSha256 = std::string(64, 'a'); // Deliberately wrong.
    manifest.packages.push_back(entry);

    fs::create_directories(fixture.backupDir() / "data");
    std::ofstream(fixture.backupDir() / "data" / "com.example.debuggable.tar.gz") << "payload";

    StandardBackend backend;
    backend.restoreAppData(AdbClient("SERIAL"), fixture.backupDir(), manifest, {});

    const std::string error = findPackageEntry(manifest, "com.example.debuggable")->error;
    ABP_CHECK(error.find("checksum mismatch") != std::string::npos);
}

ABP_TEST(standard_restore_reports_a_missing_run_as_archive) {
    FakeAdb fixture("exit 0\n");

    Manifest manifest;
    manifest.mode = "standard";
    PackageBackupEntry entry;
    entry.name = "com.example.debuggable";
    entry.dataIncluded = true;
    entry.dataCaptureMethod = DataCaptureMethod::RunAsTar;
    entry.dataArchive = "data/gone.tar.gz";
    manifest.packages.push_back(entry);

    StandardBackend backend;
    backend.restoreAppData(AdbClient("SERIAL"), fixture.backupDir(), manifest, {});

    ABP_CHECK(findPackageEntry(manifest, "com.example.debuggable")->error.find("missing") != std::string::npos);
}

ABP_TEST(standard_restore_pushes_shared_storage_into_the_sdcard_root) {
    FakeAdb fixture(R"SH(
exit 0
)SH");

    Manifest manifest;
    manifest.mode = "standard";
    manifest.sharedStorageIncluded = true;
    manifest.sharedStorageIsDirectory = true;
    manifest.sharedStorageArchive = "shared_storage";

    fs::create_directories(fixture.backupDir() / "shared_storage" / "DCIM");
    std::ofstream(fixture.backupDir() / "shared_storage" / "DCIM" / "a.jpg") << "photo";

    StandardBackend backend;
    ABP_CHECK(backend.restoreSharedStorage(AdbClient("SERIAL"), fixture.backupDir(), manifest));

    // The destination must be the parent directory. `adb push` copies a
    // source *into* a destination that already exists as a directory, so
    // naming "/sdcard/DCIM" would land the photos in /sdcard/DCIM/DCIM on
    // every device that already has a DCIM folder -- which is all of them.
    const std::string log = fixture.readLog();
    ABP_CHECK(log.find("DCIM /sdcard\n") != std::string::npos);
    ABP_CHECK(log.find("/sdcard/DCIM") == std::string::npos);
}

ABP_TEST(standard_restore_explains_a_root_mode_shared_storage_archive) {
    FakeAdb fixture("exit 0\n");

    // A root-mode backup: shared storage is a single tar file, not a tree.
    Manifest manifest;
    manifest.mode = "root";
    manifest.sharedStorageIncluded = true;
    manifest.sharedStorageIsDirectory = false;
    manifest.sharedStorageArchive = "shared_storage.tar";
    std::ofstream(fixture.backupDir() / "shared_storage.tar") << "tar";

    StandardBackend backend;
    ABP_CHECK(!backend.restoreSharedStorage(AdbClient("SERIAL"), fixture.backupDir(), manifest));
    // Nothing may be pushed, and in particular the archive must not be
    // mistaken for a directory tree.
    ABP_CHECK(fixture.readLog().find("push") == std::string::npos);
}
