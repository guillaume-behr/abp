#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "abp/Manifest.h"

namespace abp {

namespace fs = std::filesystem;

/// Headline facts about one backup directory, cheap enough to compute for
/// every backup under a root folder so the GUI can render a list without
/// loading each manifest's package array twice.
struct BackupSummaryInfo {
    fs::path path;             ///< Absolute path of the backup directory.
    std::string name;          ///< Directory name, used as the display label.
    std::string createdAtUtc;
    std::string abpVersion;
    std::string mode;          ///< "root" or "standard"

    std::string deviceSerial;
    std::string deviceModel;
    std::string deviceManufacturer;
    std::string androidRelease;

    int packageCount = 0;
    int packagesWithData = 0;
    int packagesWithErrors = 0;
    bool sharedStorageIncluded = false;

    /// Size of the backup directory on disk, recursively.
    unsigned long long diskBytes = 0;

    /// Non-empty if manifest.json exists but could not be parsed; the entry
    /// is still listed so a broken backup is visible rather than silently
    /// missing.
    std::string error;
};

/// One entry of a backup directory listing, as shown by the file browser.
struct BackupFileEntry {
    std::string name;
    std::string relativePath; ///< Relative to the backup directory root.
    bool isDirectory = false;
    unsigned long long sizeBytes = 0;
};

/// One file a backup's manifest refers to that is not as recorded.
struct VerifyProblem {
    std::string path;    ///< Relative to the backup directory.
    std::string problem; ///< e.g. "missing", "checksum mismatch".
};

/// Outcome of BackupStore::verify().
struct VerifyReport {
    int verified = 0;     ///< Files whose SHA-256 matched the manifest.
    int unverifiable = 0; ///< Present, but recorded without a checksum (APKs, pulled trees, the legacy .ab).
    std::vector<VerifyProblem> problems;

    bool ok() const { return problems.empty(); }
};

/// Read-only view over backup directories on disk: discovery, manifest
/// loading, and sandboxed browsing of a backup's contents. Nothing here
/// touches a device -- it is the "explore an existing backup" half of the
/// GUI, and is deliberately separate from BackupManager, which only knows
/// how to produce and consume backups via adb.
class BackupStore {
public:
    /// True if `dir` looks like an abp backup (i.e. contains manifest.json).
    static bool isBackupDirectory(const fs::path& dir);

    /// Reads one backup directory's summary. `error` is set on the returned
    /// value rather than thrown, so a corrupt backup still shows up.
    static BackupSummaryInfo summarize(const fs::path& dir);

    /// Finds backup directories at or below `root`, descending at most
    /// `maxDepth` levels (0 = only `root` itself). A directory that is
    /// itself a backup is not descended into. Results are sorted by
    /// creation timestamp, newest first.
    static std::vector<BackupSummaryInfo> scan(const fs::path& root, int maxDepth = 2);

    /// Loads the full manifest of the backup at `dir`. Throws
    /// std::runtime_error if it is missing or malformed.
    static Manifest loadManifest(const fs::path& dir);

    /// Checks that every file the backup's manifest lists is present inside
    /// the backup and, where a checksum was recorded, still matches it --
    /// the same checks a restore makes, without a device. Throws
    /// std::runtime_error if the manifest itself cannot be read.
    static VerifyReport verify(const fs::path& dir);

    /// Resolves `relative` inside `backupDir`, guaranteeing the result stays
    /// within the backup directory (so a browsing request can never escape
    /// it via "..", an absolute path, or a symlink). Returns false if the
    /// path escapes or does not exist.
    static bool resolveInside(const fs::path& backupDir, const std::string& relative, fs::path* resolved);

    /// Lists the direct children of `relative` inside the backup at
    /// `backupDir`, sorted directories-first then by name. Throws
    /// std::runtime_error if the path escapes the backup or is not a
    /// directory.
    static std::vector<BackupFileEntry> listDirectory(const fs::path& backupDir, const std::string& relative);
};

} // namespace abp
