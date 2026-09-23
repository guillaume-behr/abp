#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "abp/BackupOptions.h"
#include "abp/PersonalData.h"

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

    /// Device paths pulled wholesale via --all-files/--pull-path, and how
    /// many of those trees adb could only read part of.
    int filesystemCaptureCount = 0;
    int filesystemPartialCount = 0;

    /// Removable SD cards / USB drives found and pulled (counted in
    /// filesystemCaptureCount too).
    int removableStorageCount = 0;

    /// One entry per kind of personal data (contacts, SMS, ...), empty when
    /// --no-personal was given.
    std::vector<personal::ExportResult> personalExports;

    /// Packages whose private data could not be captured at all, and those
    /// that went through legacy `adb backup`, which on Android 12+ holds data
    /// only for apps that opt in.
    int packagesWithoutData = 0;

    /// Coverage caveats worth reading before relying on this backup (apps
    /// with hardware-bound secrets, app data out of reach, ...). Not errors.
    std::vector<std::string> warnings;

    unsigned long long totalBytes = 0;
    std::filesystem::path outputDir;
    std::vector<std::string> messages; ///< Fatal errors, printed to the user.
};

struct RestoreSummary {
    bool success = false;
    int packagesRestored = 0;
    int packagesFailed = 0;
    /// Selected, but the backup held nothing to write back for them -- no
    /// APK and no captured data. Counting these as restored would overstate
    /// what the run achieved.
    int packagesSkipped = 0;
    bool sharedStorageRestored = false;

    /// Raw device-path captures found in the backup. abp reports these but
    /// never writes them back -- see the note in runRestore().
    int filesystemCapturesPresent = 0;

    /// What was done with the backup's personal data exports.
    personal::RestoreResult personal;

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
