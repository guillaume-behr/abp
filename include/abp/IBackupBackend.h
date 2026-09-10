#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "abp/AdbClient.h"
#include "abp/Manifest.h"
#include "abp/Package.h"

namespace abp {

/// Strategy interface for the two ways abp can move app data and shared
/// storage between the device and the host: a root-powered tar pipeline
/// (RootBackend) or the legacy `adb backup`/pull flow (StandardBackend).
///
/// APK extraction and installation are handled by BackupManager directly,
/// since they need no elevated privileges and are identical in both modes.
class IBackupBackend {
public:
    virtual ~IBackupBackend() = default;

    /// Short, lowercase identifier stored in the manifest ("root"/"standard").
    virtual const char* name() const = 0;

    /// Captures private app data for each of `packages` into `outDir`,
    /// filling in the matching PackageBackupEntry in `manifest.packages`
    /// (which must already contain one entry per package, created by
    /// BackupManager). Packages with no data, or that fail, are left with
    /// dataIncluded == false and, on failure, a non-empty `error`.
    virtual void backupAppData(const AdbClient& adb, const std::filesystem::path& outDir,
                                const std::vector<PackageInfo>& packages, Manifest& manifest) = 0;

    /// Restores private app data described by `manifest.packages` from
    /// `backupDir`. If `packageFilter` is non-empty, only those packages are
    /// restored (backends that can only restore atomically, like the legacy
    /// adb restore flow, should log a warning and restore everything).
    virtual void restoreAppData(const AdbClient& adb, const std::filesystem::path& backupDir, Manifest& manifest,
                                 const std::vector<std::string>& packageFilter) = 0;

    /// Captures /sdcard into `outDir`, updating the sharedStorage* fields of
    /// `manifest`. Returns false if nothing was captured.
    virtual bool backupSharedStorage(const AdbClient& adb, const std::filesystem::path& outDir,
                                      Manifest& manifest) = 0;

    /// Restores /sdcard from `backupDir` per `manifest`. Returns false if
    /// the manifest has no shared storage capture or restoring it failed.
    virtual bool restoreSharedStorage(const AdbClient& adb, const std::filesystem::path& backupDir,
                                       const Manifest& manifest) = 0;
};

} // namespace abp
