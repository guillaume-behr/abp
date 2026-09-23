#include "abp/BackupManager.h"

#include <algorithm>
#include <memory>
#include <system_error>

#include "abp/AdbClient.h"
#include "abp/DevicePaths.h"
#include "abp/FsUtil.h"
#include "abp/IBackupBackend.h"
#include "abp/Logger.h"
#include "abp/Manifest.h"
#include "abp/PersonalData.h"
#include "abp/Process.h"
#include "abp/RootBackend.h"
#include "abp/StandardBackend.h"
#include "abp/StringUtil.h"
#include "abp/Version.h"

namespace abp {
namespace fs = std::filesystem;
namespace {

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

    const int total = static_cast<int>(packages.size());
    int done = 0;
    for (const auto& pkg : packages) {
        if (Process::cancelRequested()) return;
        Logger::progress("Extracting APKs", done++, total, pkg.name);
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
            entry->error = Process::cancelRequested() ? "not captured: the backup was cancelled"
                                                      : "failed to pull APK file(s) from device";
        }
    }
    Logger::progress("Extracting APKs", total, total);
}

void installApks(const AdbClient& adb, const fs::path& backupDir, Manifest& manifest,
                  const std::vector<std::string>& selectedNames) {
    const int total = static_cast<int>(selectedNames.size());
    int done = 0;
    for (auto& entry : manifest.packages) {
        if (!contains(selectedNames, entry.name)) continue;
        if (Process::cancelRequested()) return;
        Logger::progress("Installing APKs", done++, total, entry.name);
        if (!entry.apkIncluded || entry.apkFiles.empty()) continue;

        std::vector<std::string> localPaths;
        localPaths.reserve(entry.apkFiles.size());
        for (const auto& rel : entry.apkFiles) localPaths.push_back((backupDir / rel).string());

        Logger::info("Installing " + entry.name + " (" + std::to_string(localPaths.size()) + " APK file(s))...");
        if (!adb.installApks(localPaths, /*reinstall=*/true)) {
            entry.error = "failed to install APK(s)";
            Logger::warn("Failed to install " + entry.name);
        }
    }
    Logger::progress("Installing APKs", total, total);
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

    const int total = static_cast<int>(paths.size());
    int done = 0;
    for (const auto& devicePath : paths) {
        if (Process::cancelRequested()) return;
        Logger::progress("Pulling device paths", done++, total, devicePath);
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

/// Apps whose important secrets (2FA seeds, message keys, banking
/// credentials) are typically sealed by the phone's hardware keystore. Their
/// data files can be copied, but on another phone -- or after a factory
/// reset -- the keys they were encrypted with are gone.
const std::vector<std::string>& hardwareBoundApps() {
    static const std::vector<std::string> kApps = {
        "com.google.android.apps.authenticator2", // Google Authenticator
        "com.azure.authenticator",                // Microsoft Authenticator
        "com.authy.authy",                        // Twilio Authy
        "com.twofasapp",                          // 2FAS
        "com.beemdevelopment.aegis",              // Aegis
        "org.fedorahosted.freeotp",               // FreeOTP
        "org.liberty.android.freeotpplus",        // FreeOTP+
        "org.shadowice.flocke.andotp",            // andOTP
        "me.jmh.authenticatorpro",                // Authenticator Pro
        "com.duosecurity.duomobile",              // Duo Mobile
        "com.lastpass.authenticator",             // LastPass Authenticator
        "com.okta.android.auth",                  // Okta Verify
        "com.yubico.yubioath",                    // Yubico Authenticator
        "com.bitwarden.authenticator",            // Bitwarden Authenticator
        "org.thoughtcrime.securesms",             // Signal
        "com.google.android.apps.walletnfcrel",   // Google Wallet
    };
    return kApps;
}

/// Tells the user, at the end of a backup, what it cannot be relied on for.
/// A backup that silently lacks most apps' data looks exactly like a
/// complete one until the day it is restored.
void reportCoverage(const BackupOptions& options, const DeviceInfo& device, const Manifest& manifest,
                    BackupSummary& summary) {
    std::vector<std::string> sealed;
    int withoutData = 0;
    int legacy = 0;
    for (const auto& entry : manifest.packages) {
        if (contains(hardwareBoundApps(), entry.name)) sealed.push_back(entry.name);
        if (entry.dataCaptureMethod == DataCaptureMethod::LegacyAdbBackup) ++legacy;
        else if (!entry.dataIncluded && !entry.isSystemApp) ++withoutData;
    }

    if (!sealed.empty()) {
        summary.warnings.push_back(
            "These apps keep secrets sealed by this phone's hardware, so their data will not work on another "
            "phone or after a factory reset: " + strutil::join(sealed, ", ") +
            ". Use each app's own export or transfer feature (for 2FA apps: export or move your codes) before "
            "wiping this phone.");
    }

    if (options.includeAppData && manifest.mode == "standard") {
        summary.packagesWithoutData = withoutData + (device.sdkInt >= 31 ? legacy : 0);
        if (withoutData > 0) {
            summary.warnings.push_back(
                "Private data (logins, settings, in-app content) could not be captured for " +
                std::to_string(withoutData) + " app(s): without root, Android only lets abp read apps built as "
                "debuggable. Their APKs are saved; their data needs each app's own backup, or a rooted device.");
        }
        if (legacy > 0 && device.sdkInt >= 31) {
            summary.warnings.push_back(
                std::to_string(legacy) + " app(s) went through legacy 'adb backup', which on Android 12 and later "
                "contains data only for the few apps that opt in. Expect most of them to come back empty.");
        }
    }

    for (const auto& warning : summary.warnings) Logger::warn(warning);
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
    manifest.createdAtUtc = strutil::utcTimestamp();
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

    if (options.includeApks && !packages.empty() && !Process::cancelRequested()) {
        Logger::info("Extracting APKs...");
        extractApks(adb, options.outputDir, packages, manifest);
    }

    if (options.includeAppData && !packages.empty() && !Process::cancelRequested()) {
        Logger::info("Backing up app data...");
        backend->backupAppData(adb, options.outputDir, packages, manifest);
    }

    if (options.includeSharedStorage && !Process::cancelRequested()) {
        Logger::info("Backing up shared storage...");
        summary.sharedStorageIncluded = backend->backupSharedStorage(adb, options.outputDir, manifest);
        if (!summary.sharedStorageIncluded) {
            Logger::warn("Shared storage (/sdcard) could not be captured; the backup does not include it.");
        }
    }

    if (options.exportPersonalData && !Process::cancelRequested()) {
        Logger::info("Exporting contacts, messages, call log, calendar and settings...");
        summary.personalExports = personal::exportPersonalData(adb, options.outputDir, device, manifest);
    }

    // Removable SD cards are the user's files as much as /sdcard is, so they
    // come with shared storage. They are recorded as raw path captures:
    // restoring onto a phone that may not even have that card is left to
    // the user.
    std::vector<std::string> capturePaths = options.filesystemPaths;
    if (options.includeSharedStorage && !Process::cancelRequested()) {
        for (const auto& root : adb.removableStorageRoots()) {
            Logger::info("Found removable storage at " + root + "; it will be copied too.");
            capturePaths.push_back(root);
            ++summary.removableStorageCount;
        }
    }

    if (!capturePaths.empty() && !Process::cancelRequested()) {
        // `adb pull` transfers as whatever user adbd runs as. A `su` binary
        // cannot change that: su elevates commands run through the shell,
        // while pull is a separate file-transfer service. So on a Magisk-style
        // device this capture is no more complete than on an unrooted one,
        // and saying so beats letting the "root mode" banner imply otherwise.
        if (options.filesystemPaths.empty()) {
            // Only SD cards: readable by the shell user, nothing to warn about.
        } else if (device.root.method == RootMethod::SuBinary) {
            Logger::warn("This device is rooted through a 'su' binary, but 'adb pull' transfers as the adb user, "
                          "which 'su' cannot elevate. Paths like /data will be captured only in part. Run "
                          "'adb root' first for a complete capture.");
        } else if (!device.isRooted()) {
            Logger::warn("Without root, 'adb pull' can read /sdcard and the read-only system partitions but "
                          "almost nothing under /data. Incomplete trees are marked in manifest.json.");
        }

        Logger::info("Pulling device filesystem paths...");
        captureFilesystem(adb, options.outputDir, capturePaths, summary.sharedStorageIncluded, manifest);
        summary.filesystemCaptureCount = static_cast<int>(manifest.filesystemCaptures.size());
        for (const auto& capture : manifest.filesystemCaptures) {
            if (!capture.complete) ++summary.filesystemPartialCount;
        }
    }

    // Packages the cancel reached before anything was captured for them must
    // not read as clean, empty successes in the manifest.
    if (Process::cancelRequested()) {
        for (auto& entry : manifest.packages) {
            if (!entry.apkIncluded && !entry.dataIncluded && entry.error.empty()) {
                entry.error = "not captured: the backup was cancelled";
            }
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
    if (Process::cancelRequested()) {
        // What was captured before the cancel is real and now described by
        // manifest.json, so it is kept; the run still counts as failed.
        summary.cancelled = true;
        summary.messages.push_back("Backup cancelled. What was captured before that is in '" +
                                   options.outputDir.string() + "' and listed in its manifest.json.");
        summary.totalBytes = fsutil::directorySize(options.outputDir);
        return summary;
    }

    reportCoverage(options, device, manifest, summary);

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

    if (options.includeAppData && !(needsRoot && !device.isRooted()) && !Process::cancelRequested()) {
        backend->restoreAppData(adb, options.inputDir, manifest, selectedNames);
    }

    if (options.includeSharedStorage && !(needsRoot && !device.isRooted()) && !Process::cancelRequested()) {
        summary.sharedStorageRestored = backend->restoreSharedStorage(adb, options.inputDir, manifest);
    }

    // Contacts and calendar come back as files for the user to import, and
    // Wi-Fi networks are re-added; writing the providers' databases directly
    // needs root, and a root-mode backup of them is restored with the other
    // app data anyway. Messages, call log and settings are archival.
    if (options.includePersonalData && !manifest.personalDataExports.empty() && !Process::cancelRequested()) {
        summary.personal = personal::restorePersonalData(adb, options.inputDir, manifest, device.sdkInt);
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
        for (const auto& capture : manifest.filesystemCaptures) {
            if (strutil::startsWith(capture.devicePath, "/storage/") &&
                !strutil::startsWith(capture.devicePath, "/storage/emulated")) {
                Logger::info("SD card contents from " + capture.devicePath + " are in '" +
                             (options.inputDir / capture.localPath).string() +
                             "'. To put them back, insert a card and run: adb push '" +
                             (options.inputDir / capture.localPath).string() + "/.' /storage/<card-id>/");
            }
        }
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

    if (Process::cancelRequested()) {
        summary.cancelled = true;
        summary.messages.push_back("Restore cancelled; the device may hold a partial restore.");
        return summary;
    }

    summary.success = true;
    return summary;
}

} // namespace abp
