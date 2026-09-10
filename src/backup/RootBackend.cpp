#include "abp/RootBackend.h"

#include <algorithm>
#include <system_error>

#include "abp/FsUtil.h"
#include "abp/Logger.h"
#include "abp/Sha256.h"
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

void RootBackend::backupAppData(const AdbClient& adb, const fs::path& outDir,
                                 const std::vector<PackageInfo>& packages, Manifest& manifest) {
    fs::path dataDir = outDir / "data";
    fsutil::ensureDirectory(dataDir);

    for (const auto& pkg : packages) {
        PackageBackupEntry* entry = findPackageEntry(manifest, pkg.name);
        if (entry == nullptr) continue;

        if (!strutil::isValidPackageName(pkg.name)) {
            entry->error = "invalid package name, skipped";
            continue;
        }

        bool checkOk = false;
        std::string exists =
            adb.shellText(asRoot("[ -d /data/data/" + pkg.name + " ] && echo yes || echo no"), &checkOk);
        if (!checkOk || exists != "yes") {
            continue; // No private data directory; nothing to capture.
        }

        std::string fileName = fsutil::sanitizeForFilename(pkg.name) + ".tar.gz";
        fs::path localPath = dataDir / fileName;

        std::string tarCmd = asRoot("tar -czf - -C /data/data " + pkg.name + " 2>/dev/null");
        if (!adb.execOutToFile(tarCmd, localPath.string())) {
            entry->error = "failed to capture app data via tar";
            std::error_code ec;
            fs::remove(localPath, ec);
            continue;
        }

        unsigned long long size = fsutil::fileSize(localPath);
        if (size == 0) {
            std::error_code ec;
            fs::remove(localPath, ec);
            continue;
        }

        entry->dataIncluded = true;
        entry->dataArchive = (fs::path("data") / fileName).generic_string();
        entry->dataArchiveBytes = size;
        entry->dataArchiveSha256 = crypto::sha256HexFile(localPath.string());
    }
}

void RootBackend::restoreAppData(const AdbClient& adb, const fs::path& backupDir, Manifest& manifest,
                                  const std::vector<std::string>& packageFilter) {
    for (auto& entry : manifest.packages) {
        if (!entry.dataIncluded || entry.dataArchive.empty()) continue;
        if (!packageFilter.empty() &&
            std::find(packageFilter.begin(), packageFilter.end(), entry.name) == packageFilter.end()) {
            continue;
        }
        if (!strutil::isValidPackageName(entry.name)) {
            entry.error = "invalid package name, skipped restore";
            continue;
        }

        fs::path archivePath = backupDir / entry.dataArchive;
        if (!fs::exists(archivePath)) {
            entry.error = "data archive missing on disk: " + entry.dataArchive;
            continue;
        }

        if (!entry.dataArchiveSha256.empty() &&
            crypto::sha256HexFile(archivePath.string()) != entry.dataArchiveSha256) {
            entry.error = "checksum mismatch for data archive, refusing to restore";
            continue;
        }

        // Snapshot the UID/GID the package manager just assigned to this
        // (freshly (re)installed, empty) app before we overwrite its data
        // directory with the archive's original ownership.
        bool statOk = false;
        std::string owner =
            adb.shellText(asRoot("stat -c '%u:%g' /data/data/" + entry.name + " 2>/dev/null"), &statOk);

        std::string extractCmd =
            asRoot("mkdir -p /data/data/" + entry.name + " && tar -xzf - -C /data/data 2>/dev/null");
        if (!adb.shellFromFile(extractCmd, archivePath.string())) {
            entry.error = "failed to extract app data archive on device";
            continue;
        }

        if (statOk && !owner.empty()) {
            adb.shell(asRoot("chown -R " + owner + " /data/data/" + entry.name + " 2>/dev/null"));
        } else {
            Logger::warn("Could not determine target UID for " + entry.name +
                         "; restored data may have the wrong owner until the app is opened.");
        }
        adb.shell(asRoot("restorecon -R /data/data/" + entry.name + " 2>/dev/null"));
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
    manifest.sharedStorageArchiveSha256 = crypto::sha256HexFile(localPath.string());
    return true;
}

bool RootBackend::restoreSharedStorage(const AdbClient& adb, const fs::path& backupDir, const Manifest& manifest) {
    if (!manifest.sharedStorageIncluded) return false;

    fs::path archivePath = backupDir / manifest.sharedStorageArchive;
    if (!fs::exists(archivePath)) return false;

    if (!manifest.sharedStorageArchiveSha256.empty() &&
        crypto::sha256HexFile(archivePath.string()) != manifest.sharedStorageArchiveSha256) {
        Logger::error("Checksum mismatch for shared storage archive, refusing to restore.");
        return false;
    }

    std::string cmd = asRoot("tar -xf - -C /sdcard 2>/dev/null");
    return adb.shellFromFile(cmd, archivePath.string());
}

} // namespace abp
