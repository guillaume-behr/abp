#pragma once

#include "abp/IBackupBackend.h"

namespace abp {

/// Non-root backend built entirely on public adb functionality: the legacy
/// `adb backup`/`adb restore` flow for app data (which only works for apps
/// with `android:allowBackup="true"` and requires the user to confirm a
/// prompt on the device screen) and `adb pull`/`adb push` for shared
/// storage. This is what abp falls back to when no root access is
/// available.
class StandardBackend : public IBackupBackend {
public:
    const char* name() const override { return "standard"; }

    void backupAppData(const AdbClient& adb, const std::filesystem::path& outDir,
                        const std::vector<PackageInfo>& packages, Manifest& manifest) override;

    void restoreAppData(const AdbClient& adb, const std::filesystem::path& backupDir, Manifest& manifest,
                         const std::vector<std::string>& packageFilter) override;

    bool backupSharedStorage(const AdbClient& adb, const std::filesystem::path& outDir, Manifest& manifest) override;

    bool restoreSharedStorage(const AdbClient& adb, const std::filesystem::path& backupDir,
                               const Manifest& manifest) override;
};

} // namespace abp
