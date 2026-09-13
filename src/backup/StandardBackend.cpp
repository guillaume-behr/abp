#include "abp/StandardBackend.h"

#include <algorithm>
#include <system_error>

#include "abp/ArchiveIntegrity.h"
#include "abp/FsUtil.h"
#include "abp/Logger.h"
#include "abp/StringUtil.h"

namespace abp {
namespace fs = std::filesystem;

namespace {

/// Captures one package's private data as the app's own UID via `run-as`,
/// producing the same `data/<pkg>.tar.gz` layout root mode uses. Returns
/// false if nothing was captured.
bool captureViaRunAs(const AdbClient& adb, const fs::path& dataDir, const std::string& packageName,
                      PackageBackupEntry& entry) {
    const std::string fileName = fsutil::sanitizeForFilename(packageName) + ".tar.gz";
    const fs::path localPath = dataDir / fileName;

    // `-C /data/data <pkg>` (rather than `-C /data/data/<pkg> .`) keeps the
    // archive byte-for-byte the same shape as root mode's, so both restore
    // paths -- and anyone inspecting the backup by hand -- see one layout.
    const std::string tarCmd =
        AdbClient::asPackage(packageName, "tar -czf - -C /data/data " + strutil::shellQuote(packageName)) +
        " 2>/dev/null";

    if (!adb.execOutToFile(tarCmd, localPath.string())) {
        std::error_code ec;
        fs::remove(localPath, ec);
        return false;
    }

    const unsigned long long size = fsutil::fileSize(localPath);
    if (size == 0) {
        std::error_code ec;
        fs::remove(localPath, ec);
        return false;
    }

    entry.dataIncluded = true;
    entry.dataCaptureMethod = DataCaptureMethod::RunAsTar;
    entry.dataArchive = (fs::path("data") / fileName).generic_string();
    entry.dataArchiveBytes = size;
    entry.dataArchiveSha256 = integrity::checksumOrEmpty(localPath);
    return true;
}

} // namespace

void StandardBackend::backupAppData(const AdbClient& adb, const fs::path& outDir,
                                     const std::vector<PackageInfo>& packages, Manifest& manifest) {
    if (packages.empty()) return;

    std::vector<std::string> allNames;
    allNames.reserve(packages.size());
    for (const auto& pkg : packages) allNames.push_back(pkg.name);

    // Step 1: anything debuggable can be read directly through `run-as`, with
    // no on-device prompt and no dependence on `adb backup` (which Android 12
    // and later largely neutered). This is per-package, so it also makes
    // --only/--exclude work on restore.
    Logger::info("Checking which packages are reachable via 'run-as'...");
    const std::vector<std::string> runAsNames = adb.packagesSupportingRunAs(allNames);

    std::vector<std::string> remaining;
    if (!runAsNames.empty()) {
        Logger::info("Capturing " + std::to_string(runAsNames.size()) +
                     " debuggable package(s) directly via 'run-as'.");
        fsutil::ensureDirectory(outDir / "data");
    }

    for (const auto& pkg : packages) {
        PackageBackupEntry* entry = findPackageEntry(manifest, pkg.name);
        if (entry == nullptr) continue;

        const bool runAsCapable =
            std::find(runAsNames.begin(), runAsNames.end(), pkg.name) != runAsNames.end();
        if (runAsCapable && strutil::isValidPackageName(pkg.name) &&
            captureViaRunAs(adb, outDir / "data", pkg.name, *entry)) {
            continue;
        }
        remaining.push_back(pkg.name);
    }

    if (remaining.empty()) {
        Logger::info("Every selected package was captured via 'run-as'; no legacy 'adb backup' needed.");
        return;
    }

    // Step 2: everything else has to go through the legacy whole-archive
    // flow, which is all a non-root device offers for non-debuggable apps.
    const fs::path abPath = outDir / "legacy_backup.ab";

    Logger::info("Requesting a legacy 'adb backup' for the remaining " + std::to_string(remaining.size()) +
                 " package(s).");
    Logger::info("If your device shows a 'Back up my data' confirmation, unlock it and confirm now.");
    Logger::warn("Legacy 'adb backup' skips apps with android:allowBackup=\"false\", and on Android 12 and "
                 "later it also skips app data unless the app explicitly opts in. Expect gaps -- see the "
                 "coverage table in README.md.");

    // includeApk is always false here: BackupManager already extracts the
    // full APK set (including split APKs) separately, which adb backup's
    // own -apk flag cannot do.
    const bool success = adb.backupToFile(abPath.string(), remaining, /*includeApk=*/false, /*includeShared=*/false);

    if (!success) {
        for (const auto& name : remaining) {
            if (auto* entry = findPackageEntry(manifest, name)) {
                entry->error = "not debuggable (no 'run-as' access) and adb backup produced no archive "
                                "(declined on-device, or unsupported on this Android version)";
            }
        }
        std::error_code ec;
        fs::remove(abPath, ec);
        return;
    }

    manifest.legacyAdbBackupFile = "legacy_backup.ab";
    for (const auto& name : remaining) {
        if (auto* entry = findPackageEntry(manifest, name)) {
            // Best-effort: adb backup gives no per-app success signal, so we
            // optimistically mark data as included. Apps with
            // allowBackup=false will simply be absent from the archive.
            entry->dataIncluded = true;
            entry->dataCaptureMethod = DataCaptureMethod::LegacyAdbBackup;
        }
    }
}

void StandardBackend::restoreAppData(const AdbClient& adb, const fs::path& backupDir, Manifest& manifest,
                                      const std::vector<std::string>& packageFilter) {
    auto selected = [&packageFilter](const std::string& name) {
        return packageFilter.empty() ||
               std::find(packageFilter.begin(), packageFilter.end(), name) != packageFilter.end();
    };

    // Per-package `run-as` archives restore individually, so --only/--exclude
    // are honoured for them -- unlike the legacy archive below.
    int runAsRestored = 0;
    bool anyLegacySelected = false;

    for (auto& entry : manifest.packages) {
        if (!entry.dataIncluded || !selected(entry.name)) continue;

        if (entry.dataCaptureMethod == DataCaptureMethod::LegacyAdbBackup) {
            anyLegacySelected = true;
            continue;
        }
        if (entry.dataCaptureMethod != DataCaptureMethod::RunAsTar || entry.dataArchive.empty()) {
            continue;
        }

        if (!strutil::isValidPackageName(entry.name)) {
            entry.error = "invalid package name, skipped restore";
            continue;
        }

        const fs::path archivePath = backupDir / entry.dataArchive;
        if (!fs::exists(archivePath)) {
            entry.error = "data archive missing on disk: " + entry.dataArchive;
            continue;
        }
        if (!integrity::checksumMatches(archivePath, entry.dataArchiveSha256)) {
            entry.error = "checksum mismatch for data archive, refusing to restore";
            continue;
        }

        // Stop the app before replacing its data, for the same reason root
        // mode does: a live process would otherwise write over the restore.
        adb.shell("am force-stop " + strutil::shellQuote(entry.name) + " 2>/dev/null");

        // Extracting through `run-as` means tar runs as the app's own UID, so
        // every file lands owned by the app. That sidesteps the UID remapping
        // root mode has to do by hand -- a non-root tar cannot chown anyway.
        const std::string extractCmd =
            AdbClient::asPackage(entry.name, "tar -xzf - -C /data/data") + " 2>/dev/null";
        if (!adb.shellFromFile(extractCmd, archivePath.string())) {
            entry.error = "failed to extract app data archive via 'run-as' (is the app still debuggable?)";
            continue;
        }
        ++runAsRestored;
    }

    if (runAsRestored > 0) {
        Logger::info("Restored " + std::to_string(runAsRestored) + " package(s) directly via 'run-as'.");
    }

    if (manifest.legacyAdbBackupFile.empty() || !anyLegacySelected) {
        return;
    }

    const fs::path abPath = backupDir / manifest.legacyAdbBackupFile;
    if (!fs::exists(abPath)) {
        Logger::error("Legacy backup archive missing: " + abPath.string());
        for (auto& entry : manifest.packages) {
            if (entry.dataCaptureMethod == DataCaptureMethod::LegacyAdbBackup && selected(entry.name)) {
                entry.error = "legacy backup archive missing: " + manifest.legacyAdbBackupFile;
            }
        }
        return;
    }

    // BackupManager always passes an explicit selection, so a non-empty
    // filter does not by itself mean the user narrowed anything. Warn only
    // when the whole-archive restore will actually bring back more than was
    // asked for -- that is, some legacy-captured package was left out.
    bool anyLegacyDeselected = false;
    for (const auto& entry : manifest.packages) {
        if (entry.dataCaptureMethod == DataCaptureMethod::LegacyAdbBackup && !selected(entry.name)) {
            anyLegacyDeselected = true;
            break;
        }
    }
    if (anyLegacyDeselected) {
        Logger::warn("Some deselected packages share the legacy 'adb backup' archive, which 'adb restore' can "
                     "only restore as a whole -- their data will come back too. Only packages captured via "
                     "'run-as' can be restored selectively.");
    }

    Logger::info("Requesting a legacy 'adb restore'. Unlock the device and confirm 'Restore my data' now.");
    if (!adb.restoreFromFile(abPath.string())) {
        Logger::error("adb restore failed, or was declined on the device.");
        for (auto& entry : manifest.packages) {
            if (entry.dataCaptureMethod == DataCaptureMethod::LegacyAdbBackup && selected(entry.name)) {
                entry.error = "adb restore failed, or was declined on the device";
            }
        }
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

    // Check what the manifest says before what is on disk, so a root-mode
    // backup opened by the standard backend gets the explanation rather than
    // the generic "nothing to restore" a failed is_directory() would give.
    if (!manifest.sharedStorageIsDirectory) {
        Logger::error("This backup's shared storage is a tar archive captured in root mode; "
                      "it cannot be restored through the standard backend.");
        return false;
    }

    fs::path localDir = backupDir / manifest.sharedStorageArchive;
    if (!fs::exists(localDir) || !fs::is_directory(localDir)) return false;

    Logger::info("Pushing shared storage back to /sdcard.");

    // Push each top-level entry individually so contents merge into the
    // existing /sdcard rather than nesting under /sdcard/shared_storage.
    std::error_code ec;
    fs::directory_iterator it(localDir, ec);
    if (ec) {
        Logger::error("Could not read " + localDir.string() + ": " + ec.message());
        return false;
    }

    bool allOk = true;
    int pushed = 0;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            Logger::error("Could not walk " + localDir.string() + ": " + ec.message());
            return false;
        }
        // The destination is the *parent*, never "/sdcard/<name>". `adb push`
        // follows cp's rule: when the destination already exists as a
        // directory, the source is copied inside it -- so pushing DCIM to
        // "/sdcard/DCIM" on a device that already has one lands the files in
        // /sdcard/DCIM/DCIM. Naming the parent merges into the existing
        // directory, and still creates it when the device has none.
        if (!adb.push(it->path().string(), "/sdcard")) {
            Logger::error("Failed to push " + it->path().string() + " to /sdcard");
            allOk = false;
        } else {
            ++pushed;
        }
    }

    if (pushed == 0) {
        Logger::warn("Shared storage directory " + localDir.string() + " is empty; nothing was pushed.");
        return false;
    }
    return allOk;
}

} // namespace abp
