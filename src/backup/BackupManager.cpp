#include "abp/BackupManager.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <system_error>

#include "abp/AdbClient.h"
#include "abp/DevicePaths.h"
#include "abp/FsUtil.h"
#include "abp/IBackupBackend.h"
#include "abp/Logger.h"
#include "abp/Manifest.h"
#include "abp/PersonalData.h"
#include "abp/RootBackend.h"
#include "abp/StandardBackend.h"
#include "abp/StringUtil.h"
#include "abp/Version.h"

namespace abp {
namespace fs = std::filesystem;
namespace {

std::string currentUtcTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

std::vector<PackageInfo> filterPackages(std::vector<PackageInfo> all, const BackupOptions& options) {
    std::vector<PackageInfo> result;
    result.reserve(all.size());
    for (auto& pkg : all) {
        if (!options.onlyPackages.empty() && !contains(options.onlyPackages, pkg.name)) continue;
        if (contains(options.excludePackages, pkg.name)) continue;
        result.push_back(std::move(pkg));
    }

    // Silently backing up nothing because of a typo in --only is worse than
    // saying so: warn about every requested name that matched no package.
    for (const auto& requested : options.onlyPackages) {
        bool matched = false;
        for (const auto& pkg : result) {
            if (pkg.name == requested) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            Logger::warn("Requested package '" + requested +
                          "' is not installed on this device (or was excluded); skipping it.");
        }
    }
    return result;
}

void extractApks(const AdbClient& adb, const fs::path& outDir, const std::vector<PackageInfo>& packages,
                  Manifest& manifest) {
    fs::path apksDir = outDir / "apks";

    for (const auto& pkg : packages) {
        PackageBackupEntry* entry = findPackageEntry(manifest, pkg.name);
        if (entry == nullptr || pkg.apkPaths.empty()) continue;

        std::string safeName = fsutil::sanitizeForFilename(pkg.name);
        fs::path pkgDir = apksDir / safeName;
        fsutil::ensureDirectory(pkgDir);

        std::vector<std::string> localFiles;
        std::vector<std::string> usedNames;
        bool anyFailed = false;
        for (size_t i = 0; i < pkg.apkPaths.size(); ++i) {
            std::string base = fsutil::sanitizeForFilename(fs::path(pkg.apkPaths[i]).filename().string());
            if (base.empty() || base == "_") base = "split_" + std::to_string(i) + ".apk";

            // Split APKs of one package can share a basename when they live in
            // different on-device directories. Pulling both to the same local
            // name would overwrite the first and list the same file twice in
            // the manifest, which install-multiple then rejects.
            if (std::find(usedNames.begin(), usedNames.end(), base) != usedNames.end()) {
                base = "split_" + std::to_string(i) + "_" + base;
            }
            usedNames.push_back(base);

            fs::path localPath = pkgDir / base;
            if (!adb.pull(pkg.apkPaths[i], localPath.string())) {
                anyFailed = true;
                continue;
            }
            localFiles.push_back((fs::path("apks") / safeName / base).generic_string());
        }

        if (!localFiles.empty()) {
            entry->apkIncluded = true;
            entry->apkFiles = std::move(localFiles);
            if (anyFailed) {
                // Some splits came across and some did not. Reinstalling an
                // incomplete split set fails, so this must not look clean.
                entry->error = "only some APK files could be pulled; the APK set is incomplete";
            }
        } else if (anyFailed) {
            entry->error = "failed to pull APK file(s) from device";
        }
    }
}

void installApks(const AdbClient& adb, const fs::path& backupDir, Manifest& manifest,
                  const std::vector<std::string>& selectedNames) {
    for (auto& entry : manifest.packages) {
        if (!entry.apkIncluded || entry.apkFiles.empty()) continue;
        if (!contains(selectedNames, entry.name)) continue;

        std::vector<std::string> localPaths;
        localPaths.reserve(entry.apkFiles.size());
        for (const auto& rel : entry.apkFiles) localPaths.push_back((backupDir / rel).string());

        Logger::info("Installing " + entry.name + " (" + std::to_string(localPaths.size()) + " APK file(s))...");
        if (!adb.installApks(localPaths, /*reinstall=*/true)) {
            entry.error = "failed to install APK(s)";
            Logger::warn("Failed to install " + entry.name);
        }
    }
}

/// Copies whole device paths verbatim with `adb pull`, recording each tree
/// in the manifest. Needs no elevated privileges of its own -- what it can
/// read is simply whatever the adb user can see, which is far more when adbd
/// is running as root.
void captureFilesystem(const AdbClient& adb, const fs::path& outDir, const std::vector<std::string>& requestedPaths,
                        bool sharedStorageAlreadyCaptured, Manifest& manifest) {
    std::vector<std::string> paths = devicepaths::collapseRedundant(requestedPaths);
    if (paths.empty()) return;

    // sanitizeForFilename() is many-to-one: "/data/app" and "/data_app" both
    // reduce to "data_app". Two such paths would otherwise write to the same
    // directory, and since each capture wipes its destination first, the
    // second would silently replace the first while the manifest claimed both.
    std::vector<std::string> usedNames;
    auto uniqueName = [&usedNames](std::string candidate) {
        std::string name = candidate;
        for (int suffix = 2; std::find(usedNames.begin(), usedNames.end(), name) != usedNames.end(); ++suffix) {
            name = candidate + "_" + std::to_string(suffix);
        }
        usedNames.push_back(name);
        return name;
    };

    const fs::path fsDir = outDir / "filesystem";
    if (!fsutil::ensureDirectory(fsDir)) {
        Logger::error("Could not create " + fsDir.string() + "; skipping the filesystem capture.");
        return;
    }

    for (const auto& devicePath : paths) {
        const devicepaths::PathVerdict verdict = devicepaths::classify(devicePath);
        if (verdict != devicepaths::PathVerdict::Ok) {
            Logger::warn(devicepaths::explainVerdict(verdict, devicePath));
            continue;
        }

        // /sdcard is already in the backup as shared storage; pulling it again
        // would silently double the size of a full-device capture.
        if (sharedStorageAlreadyCaptured &&
            (devicepaths::isUnder(devicePath, "/sdcard") || devicepaths::isUnder(devicePath, "/storage/emulated/0"))) {
            Logger::info("Skipping " + devicePath + " (already captured as shared storage).");
            continue;
        }

        bool exists = false;
        adb.shellText("[ -e " + strutil::shellQuote(devicePath) + " ] && echo yes", &exists);
        if (!exists) {
            Logger::debug("Skipping " + devicePath + " (not present on this device).");
            continue;
        }

        // One directory per captured root, named after the path so the
        // backup is navigable: "/data/app" becomes "filesystem/data_app".
        const std::string localName = uniqueName(fsutil::sanitizeForFilename(
            devicePath.substr(1).empty() ? std::string("root") : devicePath.substr(1)));
        const fs::path localPath = fsDir / localName;

        std::error_code ec;
        fs::remove_all(localPath, ec); // adb pull creates the destination itself.

        Logger::info("Pulling " + devicePath + " ...");

        bool sawErrors = false;
        std::string errorText;
        const bool ok = adb.pullTree(devicePath, localPath.string(), &sawErrors, &errorText);

        const unsigned long long bytes = fsutil::directorySize(localPath);
        if (!ok && bytes == 0) {
            Logger::warn("Could not pull " + devicePath +
                          (errorText.empty() ? std::string() : ": " + errorText));
            fs::remove_all(localPath, ec);
            continue;
        }

        FilesystemCapture capture;
        capture.devicePath = devicePath;
        capture.localPath = (fs::path("filesystem") / localName).generic_string();
        capture.bytes = bytes;
        capture.complete = ok && !sawErrors;
        if (!capture.complete) {
            capture.note = errorText.empty()
                                ? "adb pull reported errors; parts of this tree were not readable"
                                : errorText.substr(0, 500);
            Logger::warn("Captured " + devicePath + " only partially (" + strutil::formatBytes(bytes) +
                          "); some paths were not readable by adb.");
        } else {
            Logger::info("Captured " + devicePath + " (" + strutil::formatBytes(bytes) + ").");
        }
        manifest.filesystemCaptures.push_back(std::move(capture));
    }
}

std::unique_ptr<IBackupBackend> chooseBackupBackend(const BackupOptions& options, const DeviceInfo& device) {
    bool useRoot;
    switch (options.mode) {
        case BackupMode::Root:
            useRoot = true;
            break;
        case BackupMode::Standard:
            useRoot = false;
            break;
        case BackupMode::Auto:
        default:
            useRoot = device.isRooted();
            break;
    }

    if (useRoot) return std::make_unique<RootBackend>(device.root);
    return std::make_unique<StandardBackend>();
}

} // namespace

BackupSummary BackupManager::runBackup(const BackupOptions& options) {
    BackupSummary summary;
    summary.outputDir = options.outputDir;

    AdbClient adb(options.serial);
    if (std::string problem = adb.connectionProblem(); !problem.empty()) {
        summary.messages.push_back(problem);
        return summary;
    }
    adb = adb.pinned();

    Logger::info("Querying device...");
    DeviceInfo device = adb.queryDeviceInfo();
    Logger::info("Device: " + device.manufacturer + " " + device.model + " (Android " + device.androidRelease +
                 ", SDK " + std::to_string(device.sdkInt) + ")");

    if (options.mode == BackupMode::Root && !device.isRooted()) {
        summary.messages.push_back("Root mode was requested, but no root access was detected on this device "
                                    "(no rooted adbd and no working 'su'). See docs/ROOT_BACKUP.md.");
        return summary;
    }

    std::unique_ptr<IBackupBackend> backend = chooseBackupBackend(options, device);
    Logger::info(std::string("Using backend: ") + backend->name() +
                 (options.mode == BackupMode::Auto && !device.isRooted()
                      ? " (root not available, falling back to standard mode)"
                      : ""));

    if (!fsutil::ensureDirectory(options.outputDir)) {
        summary.messages.push_back("Could not create output directory: " + options.outputDir.string());
        return summary;
    }

    std::error_code existingEc;
    if (fs::exists(options.outputDir / "manifest.json", existingEc)) {
        Logger::warn("'" + options.outputDir.string() +
                      "' already contains a backup; its manifest and any same-named archives will be overwritten.");
    }

    Manifest manifest;
    manifest.formatVersion = kManifestFormatVersion;
    manifest.abpVersion = kVersionString;
    manifest.createdAtUtc = currentUtcTimestamp();
    manifest.mode = backend->name();
    manifest.device = device;

    Logger::info("Enumerating installed packages...");
    std::vector<PackageInfo> packages = filterPackages(adb.listPackages(options.includeSystemApps), options);
    Logger::info("Found " + std::to_string(packages.size()) + " package(s) to back up.");

    if (packages.empty()) {
        // Nothing per-app to capture is only fatal when there is nothing else
        // either, or when the user named packages and none of them matched.
        // A freshly reset phone has no third-party apps at all, and its photos
        // must still be backed up.
        const bool otherCaptures =
            options.includeSharedStorage || options.exportPersonalData || !options.filesystemPaths.empty();
        if (!options.onlyPackages.empty() || !otherCaptures) {
            summary.messages.push_back("No packages matched the current selection, so there is nothing to back up. "
                                        "Check --only/--exclude, or pass --system to include system apps.");
            return summary;
        }
        Logger::warn("No packages matched the current selection; backing up only shared storage, contacts and "
                     "messages, and device paths.");
    }

    if (options.includeApks) {
        // Split APKs need `pm path`; resolve them for the selected packages
        // only, in as few on-device passes as possible.
        adb.resolveApkPaths(packages);
    }

    for (const auto& pkg : packages) {
        PackageBackupEntry entry;
        entry.name = pkg.name;
        entry.isSystemApp = pkg.isSystemApp;
        manifest.packages.push_back(std::move(entry));
    }

    if (options.includeApks && !packages.empty()) {
        Logger::info("Extracting APKs...");
        extractApks(adb, options.outputDir, packages, manifest);
    }

    if (options.includeAppData && !packages.empty()) {
        Logger::info("Backing up app data...");
        backend->backupAppData(adb, options.outputDir, packages, manifest);
    }

    if (options.includeSharedStorage) {
        Logger::info("Backing up shared storage...");
        summary.sharedStorageIncluded = backend->backupSharedStorage(adb, options.outputDir, manifest);
        if (!summary.sharedStorageIncluded) {
            Logger::warn("Shared storage (/sdcard) could not be captured; the backup does not include it.");
        }
    }

    if (options.exportPersonalData) {
        Logger::info("Exporting contacts, SMS and call log...");
        const personal::ExportCounts counts = personal::exportPersonalData(adb, options.outputDir, manifest);
        summary.contactsExported = counts.contacts;
        summary.smsExported = counts.sms;
        summary.callLogExported = counts.callLog;
    }

    if (!options.filesystemPaths.empty()) {
        // `adb pull` transfers as whatever user adbd runs as. A `su` binary
        // cannot change that: su elevates commands run through the shell,
        // while pull is a separate file-transfer service. So on a Magisk-style
        // device this capture is no more complete than on an unrooted one,
        // and saying so beats letting the "root mode" banner imply otherwise.
        if (device.root.method == RootMethod::SuBinary) {
            Logger::warn("This device is rooted through a 'su' binary, but 'adb pull' transfers as the adb user, "
                          "which 'su' cannot elevate. Paths like /data will be captured only in part. Run "
                          "'adb root' first for a complete capture.");
        } else if (!device.isRooted()) {
            Logger::warn("Without root, 'adb pull' can read /sdcard and the read-only system partitions but "
                          "almost nothing under /data. Incomplete trees are marked in manifest.json.");
        }

        Logger::info("Pulling device filesystem paths...");
        captureFilesystem(adb, options.outputDir, options.filesystemPaths, summary.sharedStorageIncluded, manifest);
        summary.filesystemCaptureCount = static_cast<int>(manifest.filesystemCaptures.size());
        for (const auto& capture : manifest.filesystemCaptures) {
            if (!capture.complete) ++summary.filesystemPartialCount;
        }
    }

    // The manifest is what makes the directory a restorable backup, so a
    // failure to write it fails the whole run rather than escaping as an
    // uncaught exception after all the data has been transferred.
    try {
        manifest.writeToFile(options.outputDir / "manifest.json");
    } catch (const std::exception& e) {
        summary.messages.push_back(std::string("Backup data was captured, but writing manifest.json failed: ") +
                                    e.what() + ". Without it the backup cannot be restored.");
        return summary;
    }

    summary.mode = manifest.mode;
    summary.packageCount = static_cast<int>(manifest.packages.size());
    for (const auto& entry : manifest.packages) {
        if (entry.dataIncluded) ++summary.packagesWithData;
        if (!entry.error.empty()) ++summary.packagesWithErrors;
        switch (entry.dataCaptureMethod) {
            case DataCaptureMethod::RootTar: ++summary.packagesCapturedByRootTar; break;
            case DataCaptureMethod::RunAsTar: ++summary.packagesCapturedByRunAs; break;
            case DataCaptureMethod::LegacyAdbBackup: ++summary.packagesCapturedByLegacyBackup; break;
            case DataCaptureMethod::None: break;
        }
    }
    // Measure the directory rather than summing the manifest's archive sizes:
    // that way APKs and the legacy .ab file are counted too.
    summary.totalBytes = fsutil::directorySize(options.outputDir);
    summary.success = true;
    return summary;
}

RestoreSummary BackupManager::runRestore(const RestoreOptions& options) {
    RestoreSummary summary;

    AdbClient adb(options.serial);
    if (std::string problem = adb.connectionProblem(); !problem.empty()) {
        summary.messages.push_back(problem);
        return summary;
    }
    adb = adb.pinned();

    fs::path manifestPath = options.inputDir / "manifest.json";
    std::error_code manifestEc;
    if (!fs::is_regular_file(manifestPath, manifestEc)) {
        summary.messages.push_back("No manifest.json found in " + options.inputDir.string() +
                                    " -- is this an abp backup directory?");
        return summary;
    }

    Manifest manifest;
    try {
        manifest = Manifest::readFromFile(manifestPath);
    } catch (const std::exception& e) {
        summary.messages.push_back(std::string("Failed to parse manifest.json: ") + e.what());
        return summary;
    }

    if (manifest.formatVersion > kManifestFormatVersion) {
        summary.messages.push_back(
            "This backup uses manifest format version " + std::to_string(manifest.formatVersion) + ", but abp " +
            kVersionString + " only understands up to version " + std::to_string(kManifestFormatVersion) +
            ". Upgrade abp rather than risk misreading a newer backup.");
        return summary;
    }
    if (manifest.formatVersion < 1) {
        summary.messages.push_back("manifest.json has no usable format_version; it does not look like an abp backup.");
        return summary;
    }

    Logger::info("Loaded backup: " + std::to_string(manifest.packages.size()) + " package(s), mode=" + manifest.mode +
                 ", captured " + manifest.createdAtUtc);

    DeviceInfo device = adb.queryDeviceInfo();
    bool needsRoot = manifest.mode == "root";
    if (needsRoot && !device.isRooted()) {
        Logger::warn("This backup was captured with root access, but the connected device does not appear to be "
                     "rooted. App data and shared storage cannot be restored -- only APKs will be reinstalled.");
    }

    std::unique_ptr<IBackupBackend> backend =
        needsRoot ? std::unique_ptr<IBackupBackend>(std::make_unique<RootBackend>(device.root))
                  : std::unique_ptr<IBackupBackend>(std::make_unique<StandardBackend>());

    std::vector<std::string> selectedNames;
    for (auto& entry : manifest.packages) {
        if (!options.onlyPackages.empty() && !contains(options.onlyPackages, entry.name)) continue;
        if (contains(options.excludePackages, entry.name)) continue;
        selectedNames.push_back(entry.name);
        // `error` currently holds whatever went wrong at *backup* time. The
        // restore steps below reuse the field, so clear it first -- otherwise
        // the summary reports old backup failures as restore failures.
        entry.error.clear();
    }

    for (const auto& requested : options.onlyPackages) {
        if (!contains(selectedNames, requested)) {
            Logger::warn("Requested package '" + requested +
                          "' is not present in this backup (or was excluded); skipping it.");
        }
    }

    if (selectedNames.empty()) {
        summary.messages.push_back("No packages in this backup matched the current selection. "
                                    "Check --only/--exclude.");
        return summary;
    }

    if (options.includeApks) {
        installApks(adb, options.inputDir, manifest, selectedNames);
    }

    if (options.includeAppData && !(needsRoot && !device.isRooted())) {
        backend->restoreAppData(adb, options.inputDir, manifest, selectedNames);
    }

    if (options.includeSharedStorage && !(needsRoot && !device.isRooted())) {
        summary.sharedStorageRestored = backend->restoreSharedStorage(adb, options.inputDir, manifest);
    }

    // Contacts come back as a vCard for the user to import: writing the
    // contacts provider's database directly is only possible with root, and
    // a root-mode backup of it is restored with the other app data anyway.
    // SMS and call log are archival -- only the default SMS app may write
    // messages -- so they are just reported.
    if (options.includePersonalData && !manifest.personalDataExports.empty()) {
        summary.contactsImportPath = personal::pushContactsForImport(adb, options.inputDir, manifest);
        if (!summary.contactsImportPath.empty()) {
            Logger::info("Copied the contacts export to " + summary.contactsImportPath +
                         ". To import it, open the Contacts app, choose Settings > Import > .vcf file, and pick "
                         "Download/abp-contacts.vcf.");
        }
        for (const auto& item : manifest.personalDataExports) {
            if (item.kind == "sms" || item.kind == "call_log") {
                Logger::info("The backup's " + item.localPath + " (" + std::to_string(item.itemCount) + " " +
                             (item.kind == "sms" ? "messages" : "calls") +
                             ") is a readable archive; Android does not let abp write it back.");
            }
        }
    }

    // Filesystem captures are deliberately not pushed back. They are raw
    // copies of whole partitions: writing /system needs a writable system
    // partition and can leave a device unbootable, and blindly pushing /data
    // over a running system would break app UIDs and SELinux labels far more
    // thoroughly than the per-package restore above. They are archival, and
    // the summary says so rather than silently ignoring them.
    if (!manifest.filesystemCaptures.empty()) {
        summary.filesystemCapturesPresent = static_cast<int>(manifest.filesystemCaptures.size());
        Logger::warn("This backup also contains " + std::to_string(manifest.filesystemCaptures.size()) +
                      " raw device path capture(s) under '" + (options.inputDir / "filesystem").string() +
                      "'. abp does not push these back: restoring whole partitions over a running system "
                      "is not safe to automate. Copy what you need from them by hand.");
    }

    // "Restored" means something was actually written back for the package.
    // Counting every selected entry would report a package the backup holds
    // nothing for -- no APK, no data -- as a successful restore, which is the
    // one thing the summary must not get wrong.
    const bool appDataAttempted = options.includeAppData && !(needsRoot && !device.isRooted());
    for (const auto& entry : manifest.packages) {
        if (!contains(selectedNames, entry.name)) continue;
        if (!entry.error.empty()) {
            ++summary.packagesFailed;
            continue;
        }
        const bool apkRestored = options.includeApks && entry.apkIncluded && !entry.apkFiles.empty();
        const bool dataRestored = appDataAttempted && entry.dataIncluded;
        if (apkRestored || dataRestored) ++summary.packagesRestored;
        else ++summary.packagesSkipped;
    }

    if (summary.packagesSkipped > 0) {
        Logger::warn(std::to_string(summary.packagesSkipped) +
                      " selected package(s) had nothing to restore in this backup (no APK and no captured data).");
    }

    summary.success = true;
    return summary;
}

} // namespace abp
