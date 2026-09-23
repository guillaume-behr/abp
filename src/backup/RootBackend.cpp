#include "abp/RootBackend.h"

#include <algorithm>
#include <system_error>

#include "abp/ArchiveIntegrity.h"
#include "abp/FsUtil.h"
#include "abp/Logger.h"
#include "abp/Process.h"
#include "abp/StringUtil.h"

namespace abp {
namespace fs = std::filesystem;

RootBackend::RootBackend(RootAccess rootAccess) : rootAccess_(rootAccess) {}

std::string RootBackend::asRoot(const std::string& command) const {
    if (rootAccess_.method == RootMethod::SuBinary) {
        return "su -c " + strutil::shellQuote(command);
    }
    return command; // AdbdRoot: the shell is already root.
}

namespace {

/// The two per-user roots an app's private data lives under. /data/data is
/// credential-encrypted storage (a symlink to /data/user/0); /data/user_de/0
/// is device-protected storage, readable before the phone is unlocked, where
/// e.g. the telephony provider keeps the SMS/MMS database.
constexpr const char* kCeRoot = "/data/data";
constexpr const char* kDeRoot = "/data/user_de/0";

} // namespace

bool RootBackend::captureTree(const AdbClient& adb, const std::string& parent, const std::string& packageName,
                              const fs::path& localPath, bool* failed) const {
    *failed = false;
    const std::string dirArg = strutil::shellQuote(parent + "/" + packageName);

    bool checkOk = false;
    std::string exists = adb.shellText(asRoot("[ -d " + dirArg + " ] && echo yes || echo no"), &checkOk);
    if (!checkOk || exists != "yes") return false; // Nothing there to capture.

    std::string tarCmd =
        asRoot("tar -czf - -C " + strutil::shellQuote(parent) + " " + strutil::shellQuote(packageName) + " 2>/dev/null");
    std::error_code ec;
    if (!adb.execOutToFile(tarCmd, localPath.string())) {
        *failed = true;
        fs::remove(localPath, ec);
        return false;
    }
    if (fsutil::fileSize(localPath) == 0) {
        fs::remove(localPath, ec);
        return false;
    }
    return true;
}

void RootBackend::backupAppData(const AdbClient& adb, const fs::path& outDir,
                                 const std::vector<PackageInfo>& packages, Manifest& manifest) {
    fs::path dataDir = outDir / "data";
    fsutil::ensureDirectory(dataDir);

    const int total = static_cast<int>(packages.size());
    int done = 0;
    for (const auto& pkg : packages) {
        if (Process::cancelRequested()) return;
        Logger::progress("Backing up app data", done++, total, pkg.name);
        PackageBackupEntry* entry = findPackageEntry(manifest, pkg.name);
        if (entry == nullptr) continue;

        if (!strutil::isValidPackageName(pkg.name)) {
            entry->error = "invalid package name, skipped";
            continue;
        }

        const std::string safeName = fsutil::sanitizeForFilename(pkg.name);

        const std::string ceName = safeName + ".tar.gz";
        bool ceFailed = false;
        if (captureTree(adb, kCeRoot, pkg.name, dataDir / ceName, &ceFailed)) {
            entry->dataArchive = (fs::path("data") / ceName).generic_string();
            entry->dataArchiveBytes = fsutil::fileSize(dataDir / ceName);
            entry->dataArchiveSha256 = integrity::checksumOrEmpty(dataDir / ceName);
        }

        const std::string deName = safeName + ".de.tar.gz";
        bool deFailed = false;
        if (captureTree(adb, kDeRoot, pkg.name, dataDir / deName, &deFailed)) {
            entry->deDataArchive = (fs::path("data") / deName).generic_string();
            entry->deDataArchiveBytes = fsutil::fileSize(dataDir / deName);
            entry->deDataArchiveSha256 = integrity::checksumOrEmpty(dataDir / deName);
        }

        if (ceFailed || deFailed) {
            entry->error = std::string("failed to capture ") +
                           (ceFailed && deFailed ? "app data" : ceFailed ? "app data (/data/data)"
                                                                         : "device-protected data (/data/user_de)") +
                           " via tar";
        }
        if (!entry->dataArchive.empty() || !entry->deDataArchive.empty()) {
            entry->dataIncluded = true;
            entry->dataCaptureMethod = DataCaptureMethod::RootTar;
        }
    }
    Logger::progress("Backing up app data", total, total);
}

std::string RootBackend::restoreTree(const AdbClient& adb, const std::string& parent, const std::string& packageName,
                                     const fs::path& archivePath) const {
    const std::string dirArg = strutil::shellQuote(parent + "/" + packageName);

    // Snapshot the UID/GID the package manager assigned to this (freshly
    // (re)installed) app before its directory is overwritten with the
    // archive's original ownership.
    bool statOk = false;
    std::string owner = adb.shellText(asRoot("stat -c '%u:%g' " + dirArg + " 2>/dev/null"), &statOk);

    // No directory means the package is not installed (its APK was not
    // restored, or failed to install). Extracting anyway would leave a
    // root-owned directory that no app UID can use, and that the package
    // manager then trips over when the app is installed later.
    if (!statOk || owner.empty() || owner.find(':') == std::string::npos) {
        return "missing";
    }

    std::string extractCmd = asRoot("tar -xzf - -C " + strutil::shellQuote(parent) + " 2>/dev/null");
    if (!adb.shellFromFile(extractCmd, archivePath.string())) return "extract";

    adb.shell(asRoot("chown -R " + strutil::shellQuote(owner) + " " + dirArg + " 2>/dev/null"));
    adb.shell(asRoot("restorecon -R " + dirArg + " 2>/dev/null"));
    return std::string();
}

void RootBackend::restoreAppData(const AdbClient& adb, const fs::path& backupDir, Manifest& manifest,
                                  const std::vector<std::string>& packageFilter) {
    bool restoredSystemApp = false;
    const int total = static_cast<int>(packageFilter.size());
    int done = 0;

    for (auto& entry : manifest.packages) {
        if (!entry.dataIncluded || (entry.dataArchive.empty() && entry.deDataArchive.empty())) continue;
        if (!packageFilter.empty() &&
            std::find(packageFilter.begin(), packageFilter.end(), entry.name) == packageFilter.end()) {
            continue;
        }
        if (Process::cancelRequested()) break;
        Logger::progress("Restoring app data", done++, total, entry.name);
        if (!strutil::isValidPackageName(entry.name)) {
            entry.error = "invalid package name, skipped restore";
            continue;
        }

        // Verify every archive before touching the device, so a corrupted
        // half never leaves the app with only the other half restored.
        bool verified = true;
        for (const std::string* archive : {&entry.dataArchive, &entry.deDataArchive}) {
            if (archive->empty()) continue;
            const fs::path path = backupDir / *archive;
            const std::string& expected = archive == &entry.dataArchive ? entry.dataArchiveSha256
                                                                        : entry.deDataArchiveSha256;
            if (!fs::exists(path)) {
                entry.error = "data archive missing on disk: " + *archive;
                verified = false;
                break;
            }
            if (!integrity::checksumMatches(path, expected)) {
                entry.error = "checksum mismatch for " + *archive + ", refusing to restore";
                verified = false;
                break;
            }
        }
        if (!verified) continue;

        // Stop the app first: extracting over the data directory of a running
        // process leaves it with a half-old, half-new view of its own files,
        // and anything it writes afterwards can clobber the restore.
        adb.shell(asRoot("am force-stop " + strutil::shellQuote(entry.name) + " 2>/dev/null"));

        if (!entry.dataArchive.empty()) {
            const std::string failure = restoreTree(adb, kCeRoot, entry.name, backupDir / entry.dataArchive);
            if (failure == "missing") {
                entry.error = "app is not installed on the device, so its data cannot be restored "
                              "(restore its APK too, or install it first)";
                continue;
            }
            if (!failure.empty()) {
                entry.error = "failed to extract app data archive on device";
                continue;
            }
        }

        if (!entry.deDataArchive.empty()) {
            const std::string failure = restoreTree(adb, kDeRoot, entry.name, backupDir / entry.deDataArchive);
            if (failure == "missing") {
                entry.error = entry.dataArchive.empty()
                                  ? "app is not installed on the device, so its data cannot be restored "
                                    "(restore its APK too, or install it first)"
                                  : "device-protected data not restored: " + std::string(kDeRoot) + "/" +
                                        entry.name + " does not exist on the device";
                continue;
            }
            if (!failure.empty()) {
                entry.error = "failed to extract device-protected data archive on device";
                continue;
            }
        }

        if (entry.isSystemApp) restoredSystemApp = true;
    }

    Logger::progress("Restoring app data", total, total);

    // System providers (contacts, SMS, ...) run inside persistent system
    // processes that `am force-stop` does not stop, and they keep their
    // databases open. They only reliably pick up restored files after a
    // restart of those processes.
    if (restoredSystemApp) {
        Logger::warn("Data of system apps was restored. Reboot the device (adb reboot) before using it, so "
                     "system services such as contacts and SMS reload their databases.");
    }
}

bool RootBackend::backupSharedStorage(const AdbClient& adb, const fs::path& outDir, Manifest& manifest) {
    fs::path localPath = outDir / "shared_storage.tar";
    std::string cmd = asRoot("tar -cf - -C /sdcard . 2>/dev/null");
    if (!adb.execOutToFile(cmd, localPath.string())) {
        std::error_code ec;
        fs::remove(localPath, ec);
        return false;
    }

    unsigned long long size = fsutil::fileSize(localPath);
    if (size == 0) {
        std::error_code ec;
        fs::remove(localPath, ec);
        return false;
    }

    manifest.sharedStorageIncluded = true;
    manifest.sharedStorageIsDirectory = false;
    manifest.sharedStorageArchive = "shared_storage.tar";
    manifest.sharedStorageArchiveBytes = size;
    manifest.sharedStorageArchiveSha256 = integrity::checksumOrEmpty(localPath);
    return true;
}

bool RootBackend::restoreSharedStorage(const AdbClient& adb, const fs::path& backupDir, const Manifest& manifest) {
    if (!manifest.sharedStorageIncluded) return false;

    // What the manifest says comes first: a standard-mode backup opened by
    // the root backend must get the explanation below, not the silent false
    // a missing-file check would return for a directory capture.
    if (manifest.sharedStorageIsDirectory) {
        Logger::error("This backup's shared storage is a pulled directory tree, not a tar archive; "
                      "it cannot be restored through the root backend.");
        return false;
    }

    fs::path archivePath = backupDir / manifest.sharedStorageArchive;
    if (!fs::exists(archivePath)) return false;

    if (!integrity::checksumMatches(archivePath, manifest.sharedStorageArchiveSha256)) {
        Logger::error("Checksum mismatch for shared storage archive, refusing to restore.");
        return false;
    }

    std::string cmd = asRoot("tar -xf - -C /sdcard 2>/dev/null");
    return adb.shellFromFile(cmd, archivePath.string());
}

} // namespace abp
