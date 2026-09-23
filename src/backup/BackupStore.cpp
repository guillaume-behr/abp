#include "abp/BackupStore.h"

#include <algorithm>
#include <stdexcept>
#include <system_error>

#include "abp/ArchiveIntegrity.h"
#include "abp/FsUtil.h"

namespace abp {
namespace {

constexpr const char* kManifestName = "manifest.json";

void collect(const fs::path& dir, int depthLeft, std::vector<BackupSummaryInfo>& out) {
    if (BackupStore::isBackupDirectory(dir)) {
        out.push_back(BackupStore::summarize(dir));
        return; // A backup directory is a leaf; never descend into its archives.
    }
    if (depthLeft <= 0) return;

    // Advanced with increment(ec) rather than a range-for: the range-for's
    // operator++ throws on an I/O error partway through a directory, which
    // would abort the whole scan (and the GUI request) over one bad folder.
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code entryEc;
        if (!it->is_directory(entryEc) || entryEc) continue;
        collect(it->path(), depthLeft - 1, out);
    }
}

} // namespace

bool BackupStore::isBackupDirectory(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && fs::is_regular_file(dir / kManifestName, ec);
}

BackupSummaryInfo BackupStore::summarize(const fs::path& dir) {
    BackupSummaryInfo info;
    std::error_code ec;
    info.path = fs::absolute(dir, ec);
    if (ec) info.path = dir;
    info.name = info.path.filename().string();
    if (info.name.empty()) info.name = info.path.string();
    info.diskBytes = fsutil::directorySize(info.path);

    try {
        Manifest manifest = Manifest::readFromFile(info.path / kManifestName);
        info.createdAtUtc = manifest.createdAtUtc;
        info.abpVersion = manifest.abpVersion;
        info.mode = manifest.mode;
        info.deviceSerial = manifest.device.serial;
        info.deviceModel = manifest.device.model;
        info.deviceManufacturer = manifest.device.manufacturer;
        info.androidRelease = manifest.device.androidRelease;
        info.packageCount = static_cast<int>(manifest.packages.size());
        info.sharedStorageIncluded = manifest.sharedStorageIncluded;
        for (const auto& entry : manifest.packages) {
            if (entry.dataIncluded) ++info.packagesWithData;
            if (!entry.error.empty()) ++info.packagesWithErrors;
        }
    } catch (const std::exception& e) {
        info.error = e.what();
    }
    return info;
}

std::vector<BackupSummaryInfo> BackupStore::scan(const fs::path& root, int maxDepth) {
    std::vector<BackupSummaryInfo> found;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return found;

    collect(root, maxDepth, found);

    // Newest first; backups without a usable timestamp sort last by name.
    std::sort(found.begin(), found.end(), [](const BackupSummaryInfo& a, const BackupSummaryInfo& b) {
        if (a.createdAtUtc != b.createdAtUtc) return a.createdAtUtc > b.createdAtUtc;
        return a.name < b.name;
    });
    return found;
}

Manifest BackupStore::loadManifest(const fs::path& dir) {
    fs::path manifestPath = dir / kManifestName;
    std::error_code ec;
    if (!fs::is_regular_file(manifestPath, ec) || ec) {
        throw std::runtime_error("No " + std::string(kManifestName) + " in " + dir.string() +
                                 " -- is this an abp backup directory?");
    }
    return Manifest::readFromFile(manifestPath);
}

VerifyReport BackupStore::verify(const fs::path& dir) {
    const Manifest manifest = loadManifest(dir);
    VerifyReport report;

    // Every path is resolved inside the backup first, so a manifest naming
    // "../elsewhere" is reported rather than followed.
    auto check = [&](const std::string& relative, const std::string& sha256) {
        if (relative.empty()) return;
        fs::path resolved;
        if (!resolveInside(dir, relative, &resolved)) {
            report.problems.push_back({relative, "missing"});
            return;
        }
        if (sha256.empty()) {
            ++report.unverifiable;
        } else if (integrity::checksumMatches(resolved, sha256)) {
            ++report.verified;
        } else {
            report.problems.push_back({relative, "checksum mismatch"});
        }
    };

    for (const auto& entry : manifest.packages) {
        for (const auto& apk : entry.apkFiles) check(apk, std::string());
        check(entry.dataArchive, entry.dataArchiveSha256);
        check(entry.deDataArchive, entry.deDataArchiveSha256);
        check(entry.externalDataArchive, entry.externalDataArchiveSha256);
    }
    if (manifest.sharedStorageIncluded) check(manifest.sharedStorageArchive, manifest.sharedStorageArchiveSha256);
    check(manifest.legacyAdbBackupFile, std::string());
    for (const auto& capture : manifest.filesystemCaptures) check(capture.localPath, std::string());
    for (const auto& item : manifest.personalDataExports) check(item.localPath, item.sha256);
    return report;
}

bool BackupStore::resolveInside(const fs::path& backupDir, const std::string& relative, fs::path* resolved) {
    std::error_code ec;
    fs::path base = fs::weakly_canonical(backupDir, ec);
    if (ec) return false;

    fs::path requested(relative);
    if (requested.is_absolute()) return false;

    // weakly_canonical collapses ".." lexically *and* resolves symlinks, so a
    // symlinked archive pointing outside the backup is rejected too.
    fs::path candidate = fs::weakly_canonical(base / requested, ec);
    if (ec) return false;

    auto baseIt = base.begin();
    auto candidateIt = candidate.begin();
    for (; baseIt != base.end(); ++baseIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *candidateIt != *baseIt) return false;
    }

    if (!fs::exists(candidate, ec) || ec) return false;
    if (resolved != nullptr) *resolved = candidate;
    return true;
}

std::vector<BackupFileEntry> BackupStore::listDirectory(const fs::path& backupDir, const std::string& relative) {
    fs::path target;
    if (!resolveInside(backupDir, relative, &target)) {
        throw std::runtime_error("Path is not inside the backup directory: " + relative);
    }

    std::error_code ec;
    if (!fs::is_directory(target, ec) || ec) {
        throw std::runtime_error("Not a directory: " + relative);
    }

    fs::path base = fs::weakly_canonical(backupDir, ec);
    std::vector<BackupFileEntry> entries;
    fs::directory_iterator it(target, fs::directory_options::skip_permission_denied, ec);
    if (ec) throw std::runtime_error("Cannot read directory " + relative + ": " + ec.message());
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path& itemPath = it->path();
        std::error_code entryEc;
        BackupFileEntry entry;
        entry.name = itemPath.filename().string();
        entry.relativePath = fs::relative(itemPath, base, entryEc).generic_string();
        entry.isDirectory = it->is_directory(entryEc);
        entry.sizeBytes = entry.isDirectory ? fsutil::directorySize(itemPath) : fsutil::fileSize(itemPath);
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const BackupFileEntry& a, const BackupFileEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return a.name < b.name;
    });
    return entries;
}

} // namespace abp
