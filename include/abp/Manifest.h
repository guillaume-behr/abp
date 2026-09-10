#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "abp/Device.h"

namespace abp {

/// Per-package record of what was captured and where, stored in the
/// manifest so a later restore knows exactly what to do (and a human can
/// inspect the backup without running abp at all).
struct PackageBackupEntry {
    std::string name;
    bool isSystemApp = false;

    bool apkIncluded = false;
    std::vector<std::string> apkFiles; ///< Paths relative to the backup directory.

    bool dataIncluded = false;
    std::string dataArchive; ///< Relative path, e.g. "data/com.example.app.tar.gz".
    unsigned long long dataArchiveBytes = 0;
    std::string dataArchiveSha256;

    bool externalDataIncluded = false;
    std::string externalDataArchive; ///< /sdcard/Android/{data,obb}/<pkg> capture, if any.
    unsigned long long externalDataArchiveBytes = 0;
    std::string externalDataArchiveSha256;

    /// Non-empty if this package could not be fully backed up; it is kept
    /// in the manifest (rather than dropped) so the summary can report it.
    std::string error;
};

/// Full description of one abp backup: device identity, the mode used to
/// produce it, and every package/shared-storage archive it contains. This
/// is what gets serialized to `manifest.json` at the root of a backup
/// directory. See docs/MANIFEST.md for the on-disk schema.
struct Manifest {
    int formatVersion = 1;
    std::string abpVersion;
    std::string createdAtUtc;
    std::string mode; ///< "root" or "standard"

    DeviceInfo device;

    bool sharedStorageIncluded = false;
    /// If true, sharedStorageArchive names a directory tree (standard mode,
    /// which has no way to tar on-device); otherwise it names a single tar
    /// file (root mode).
    bool sharedStorageIsDirectory = false;
    std::string sharedStorageArchive;
    unsigned long long sharedStorageArchiveBytes = 0;
    std::string sharedStorageArchiveSha256; ///< Empty when sharedStorageIsDirectory is true.

    /// Set only in standard mode when the legacy `adb backup` flow was used
    /// for app data instead of (or in addition to) per-package archives.
    std::string legacyAdbBackupFile;

    std::vector<PackageBackupEntry> packages;

    std::string toJson() const;
    static Manifest fromJson(const std::string& text);

    void writeToFile(const std::filesystem::path& path) const;
    static Manifest readFromFile(const std::filesystem::path& path);
};

/// Finds the entry for `packageName`, or nullptr if it is not present.
PackageBackupEntry* findPackageEntry(Manifest& manifest, const std::string& packageName);
const PackageBackupEntry* findPackageEntry(const Manifest& manifest, const std::string& packageName);

} // namespace abp
