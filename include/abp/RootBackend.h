#pragma once

#include "abp/Device.h"
#include "abp/IBackupBackend.h"

namespace abp {

/// Root-powered backend: streams `tar` archives of each package's private
/// data directory (and of /sdcard) off the device via `adb exec-out`, and
/// back on via `adb shell ... < file`. Requires either an already-root
/// adbd or a working `su` binary (see Device.h / RootAccess).
///
/// See docs/ROOT_BACKUP.md for the exact on-device commands used and the
/// UID-remapping/SELinux relabeling performed on restore.
class RootBackend : public IBackupBackend {
public:
    explicit RootBackend(RootAccess rootAccess);

    const char* name() const override { return "root"; }

    void backupAppData(const AdbClient& adb, const std::filesystem::path& outDir,
                        const std::vector<PackageInfo>& packages, Manifest& manifest) override;

    void restoreAppData(const AdbClient& adb, const std::filesystem::path& backupDir, Manifest& manifest,
                         const std::vector<std::string>& packageFilter) override;

    bool backupSharedStorage(const AdbClient& adb, const std::filesystem::path& outDir, Manifest& manifest) override;

    bool restoreSharedStorage(const AdbClient& adb, const std::filesystem::path& backupDir,
                               const Manifest& manifest) override;

private:
    /// Wraps `command` so it executes with root privileges given the
    /// detected root method (a no-op if adbd itself is already root).
    std::string asRoot(const std::string& command) const;

    /// Tars `parent/packageName` into `localPath`. Returns true if a
    /// non-empty archive was written; `*failed` is set when the directory
    /// existed but could not be captured.
    bool captureTree(const AdbClient& adb, const std::string& parent, const std::string& packageName,
                     const std::filesystem::path& localPath, bool* failed) const;

    /// Extracts `archivePath` under `parent` and fixes ownership/SELinux
    /// labels of `parent/packageName`. Returns "" on success, "missing" if
    /// that directory does not exist (package not installed), or "extract".
    std::string restoreTree(const AdbClient& adb, const std::string& parent, const std::string& packageName,
                            const std::filesystem::path& archivePath) const;

    RootAccess rootAccess_;
};

} // namespace abp
