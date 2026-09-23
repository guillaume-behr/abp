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

    /// Device paths pulled wholesale via --all-files/--pull-path, and how
    /// many of those trees adb could only read part of.
    int filesystemCaptureCount = 0;
    int filesystemPartialCount = 0;

    /// Items exported per kind; -1 when that kind was not exported (not
    /// requested, or the device refused access).
    int contactsExported = -1;
    int smsExported = -1;
    int callLogExported = -1;

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

    /// Where contacts.vcf was copied on the device for import; empty if not.
    std::string contactsImportPath;

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
