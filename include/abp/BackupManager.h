#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "abp/BackupOptions.h"

namespace abp {

struct BackupSummary {
    bool success = false;
    std::string mode; ///< "root" or "standard"
    int packageCount = 0;
    int packagesWithData = 0;
    int packagesWithErrors = 0;

    /// How the captured app data was obtained. In standard mode these split
    /// per package, so the summary can say what coverage was actually
    /// achieved rather than just naming the backend.
    int packagesCapturedByRootTar = 0;
    int packagesCapturedByRunAs = 0;
    int packagesCapturedByLegacyBackup = 0;
    bool sharedStorageIncluded = false;
    unsigned long long totalBytes = 0;
    std::filesystem::path outputDir;
    std::vector<std::string> messages; ///< Fatal errors, printed to the user.
};

struct RestoreSummary {
    bool success = false;
    int packagesRestored = 0;
    int packagesFailed = 0;
    bool sharedStorageRestored = false;
    std::vector<std::string> messages;
};

/// Orchestrates a full backup or restore: connects to the device, picks a
/// backend (root vs. standard), enumerates/filters packages, and drives
/// APK extraction/installation plus per-backend app-data and shared-storage
/// transfer, writing/reading the manifest along the way.
class BackupManager {
public:
    static BackupSummary runBackup(const BackupOptions& options);
    static RestoreSummary runRestore(const RestoreOptions& options);
};

} // namespace abp
