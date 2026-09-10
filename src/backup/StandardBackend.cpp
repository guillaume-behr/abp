#include "abp/StandardBackend.h"

#include <system_error>

#include "abp/FsUtil.h"
#include "abp/Logger.h"

namespace abp {
namespace fs = std::filesystem;

void StandardBackend::backupAppData(const AdbClient& adb, const fs::path& outDir,
                                     const std::vector<PackageInfo>& packages, Manifest& manifest) {
    if (packages.empty()) return;

    fs::path abPath = outDir / "legacy_backup.ab";
    std::vector<std::string> names;
    names.reserve(packages.size());
    for (const auto& pkg : packages) names.push_back(pkg.name);

    Logger::info("Requesting a legacy 'adb backup' for " + std::to_string(names.size()) + " package(s).");
    Logger::info("If your device shows a 'Back up my data' confirmation, unlock it and confirm now.");
    Logger::info("Note: apps with android:allowBackup=\"false\" (the modern default) are silently skipped by this mechanism.");

    // includeApk is always false here: BackupManager already extracts the
    // full APK set (including split APKs) separately, which adb backup's
    // own -apk flag cannot do.
    bool success = adb.backupToFile(abPath.string(), names, /*includeApk=*/false, /*includeShared=*/false);

    if (!success) {
        for (const auto& pkg : packages) {
            if (auto* entry = findPackageEntry(manifest, pkg.name)) {
                entry->error = "adb backup produced no archive (declined on-device, or adb backup is unsupported)";
            }
        }
        std::error_code ec;
        fs::remove(abPath, ec);
        return;
    }

    manifest.legacyAdbBackupFile = "legacy_backup.ab";
    for (const auto& pkg : packages) {
        if (auto* entry = findPackageEntry(manifest, pkg.name)) {
            // Best-effort: adb backup gives no per-app success signal, so we
            // optimistically mark data as included. Apps with
            // allowBackup=false will simply be absent from the archive.
            entry->dataIncluded = true;
        }
    }
}

void StandardBackend::restoreAppData(const AdbClient& adb, const fs::path& backupDir, Manifest& manifest,
                                      const std::vector<std::string>& packageFilter) {
    if (manifest.legacyAdbBackupFile.empty()) return;

    fs::path abPath = backupDir / manifest.legacyAdbBackupFile;
    if (!fs::exists(abPath)) {
        Logger::error("Legacy backup archive missing: " + abPath.string());
        return;
    }

    if (!packageFilter.empty()) {
        Logger::warn("Standard-mode app data restores the whole legacy backup archive at once; "
                     "per-package filtering is not supported by 'adb restore'.");
    }

    Logger::info("Requesting a legacy 'adb restore'. Unlock the device and confirm 'Restore my data' now.");
    if (!adb.restoreFromFile(abPath.string())) {
        Logger::error("adb restore failed, or was declined on the device.");
    }
}

bool StandardBackend::backupSharedStorage(const AdbClient& adb, const fs::path& outDir, Manifest& manifest) {
    fs::path localDir = outDir / "shared_storage";
    std::error_code ec;
    fs::remove_all(localDir, ec); // adb pull creates the destination itself.

    Logger::info("Pulling shared storage from /sdcard (this can take a while for large media libraries).");
    if (!adb.pull("/sdcard", localDir.string())) {
        return false;
    }

    unsigned long long size = fsutil::directorySize(localDir);
    if (size == 0) return false;

    manifest.sharedStorageIncluded = true;
    manifest.sharedStorageIsDirectory = true;
    manifest.sharedStorageArchive = "shared_storage";
    manifest.sharedStorageArchiveBytes = size;
    manifest.sharedStorageArchiveSha256.clear(); // Not meaningful for a directory tree.
    return true;
}

bool StandardBackend::restoreSharedStorage(const AdbClient& adb, const fs::path& backupDir,
                                            const Manifest& manifest) {
    if (!manifest.sharedStorageIncluded) return false;

    fs::path localDir = backupDir / manifest.sharedStorageArchive;
    if (!fs::exists(localDir) || !fs::is_directory(localDir)) return false;

    Logger::info("Pushing shared storage back to /sdcard.");

    // Push each top-level entry individually so contents merge into the
    // existing /sdcard rather than nesting under /sdcard/shared_storage.
    bool allOk = true;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(localDir, ec)) {
        std::string remoteTarget = "/sdcard/" + entry.path().filename().string();
        if (!adb.push(entry.path().string(), remoteTarget)) {
            Logger::error("Failed to push " + entry.path().string() + " to " + remoteTarget);
            allOk = false;
        }
    }
    return allOk;
}

} // namespace abp
