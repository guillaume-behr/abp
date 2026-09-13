#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace abp {

enum class BackupMode {
    Auto,      ///< Use root if available, otherwise fall back to standard.
    Root,      ///< Require root; fail if not available.
    Standard,  ///< Force the non-root `adb backup`/pull based path.
};

struct BackupOptions {
    std::string serial;
    std::filesystem::path outputDir;

    bool includeApks = true;
    bool includeAppData = true;
    bool includeSharedStorage = true;
    bool includeSystemApps = false;

    std::vector<std::string> onlyPackages;    ///< If non-empty, restrict to these.
    std::vector<std::string> excludePackages; ///< Skip these even if selected above.

    /// Whole device paths to copy verbatim with `adb pull`, on top of the
    /// per-app and shared-storage captures. Empty means no filesystem
    /// capture at all; --all-files fills this with
    /// devicepaths::defaultCaptureRoots().
    ///
    /// How much of these is actually readable depends on the device: without
    /// root the shell user sees /sdcard and the read-only system partitions
    /// but almost nothing under /data.
    std::vector<std::string> filesystemPaths;

    BackupMode mode = BackupMode::Auto;
    bool assumeYes = false; ///< Skip interactive confirmations.
};

struct RestoreOptions {
    std::string serial;
    std::filesystem::path inputDir;

    bool includeApks = true;
    bool includeAppData = true;
    bool includeSharedStorage = true;

    std::vector<std::string> onlyPackages;
    std::vector<std::string> excludePackages;

    // Note: there is no restore-time backend override. The backup's own
    // manifest.json records whether it was captured in root or standard
    // mode, and that is what determines how its app data must be restored.
    bool assumeYes = false;
};

} // namespace abp
