#include "abp/BackupManager.h"

#include <algorithm>
#include <ctime>
#include <memory>

#include "abp/AdbClient.h"
#include "abp/FsUtil.h"
#include "abp/IBackupBackend.h"
#include "abp/Logger.h"
#include "abp/Manifest.h"
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
        bool anyFailed = false;
        for (size_t i = 0; i < pkg.apkPaths.size(); ++i) {
            std::string base = fs::path(pkg.apkPaths[i]).filename().string();
            if (base.empty()) base = "split_" + std::to_string(i) + ".apk";

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
    if (!adb.isConnected()) {
        summary.messages.push_back("No connected and authorized device found" +
                                    (options.serial.empty() ? std::string() : " with serial '" + options.serial + "'") +
                                    ". Run 'abp devices' to check.");
        return summary;
    }

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

    Manifest manifest;
    manifest.abpVersion = kVersionString;
    manifest.createdAtUtc = currentUtcTimestamp();
    manifest.mode = backend->name();
    manifest.device = device;

    Logger::info("Enumerating installed packages...");
    std::vector<PackageInfo> packages = filterPackages(adb.listPackages(options.includeSystemApps), options);
    Logger::info("Found " + std::to_string(packages.size()) + " package(s) to back up.");

    for (const auto& pkg : packages) {
        PackageBackupEntry entry;
        entry.name = pkg.name;
        entry.isSystemApp = pkg.isSystemApp;
        manifest.packages.push_back(std::move(entry));
    }

    if (options.includeApks) {
        Logger::info("Extracting APKs...");
        extractApks(adb, options.outputDir, packages, manifest);
    }

    if (options.includeAppData) {
        Logger::info("Backing up app data...");
        backend->backupAppData(adb, options.outputDir, packages, manifest);
    }

    if (options.includeSharedStorage) {
        Logger::info("Backing up shared storage...");
        summary.sharedStorageIncluded = backend->backupSharedStorage(adb, options.outputDir, manifest);
    }

    manifest.writeToFile(options.outputDir / "manifest.json");

    summary.success = true;
    summary.mode = manifest.mode;
    summary.packageCount = static_cast<int>(manifest.packages.size());
    unsigned long long totalBytes = manifest.sharedStorageArchiveBytes;
    for (const auto& entry : manifest.packages) {
        if (entry.dataIncluded) ++summary.packagesWithData;
        if (!entry.error.empty()) ++summary.packagesWithErrors;
        totalBytes += entry.dataArchiveBytes + entry.externalDataArchiveBytes;
    }
    summary.totalBytes = totalBytes;
    return summary;
}

RestoreSummary BackupManager::runRestore(const RestoreOptions& options) {
    RestoreSummary summary;

    AdbClient adb(options.serial);
    if (!adb.isConnected()) {
        summary.messages.push_back("No connected and authorized device found" +
                                    (options.serial.empty() ? std::string() : " with serial '" + options.serial + "'") +
                                    ". Run 'abp devices' to check.");
        return summary;
    }

    fs::path manifestPath = options.inputDir / "manifest.json";
    if (!fs::exists(manifestPath)) {
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
    for (const auto& entry : manifest.packages) {
        if (!options.onlyPackages.empty() && !contains(options.onlyPackages, entry.name)) continue;
        if (contains(options.excludePackages, entry.name)) continue;
        selectedNames.push_back(entry.name);
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

    for (const auto& entry : manifest.packages) {
        if (!contains(selectedNames, entry.name)) continue;
        if (!entry.error.empty()) ++summary.packagesFailed;
        else ++summary.packagesRestored;
    }

    summary.success = true;
    return summary;
}

} // namespace abp
