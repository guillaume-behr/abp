#pragma once

#include <string>
#include <vector>

#include "abp/Device.h"
#include "abp/Package.h"
#include "abp/Process.h"

namespace abp {

/// Thin wrapper over the `adb` command-line tool. Every method shells out
/// to a real `adb` process (never talks to the ADB protocol directly), so
/// abp works with whatever adb the user has installed.
///
/// Commands that must run "as root" on the device are given a single,
/// pre-quoted shell command string by the caller (see StringUtil::shellQuote)
/// rather than building shell strings here, keeping this class free of
/// backup-policy decisions.
class AdbClient {
public:
    explicit AdbClient(std::string serial = "");

    /// Overrides the adb executable to invoke (default: "adb", resolved via PATH).
    static void setAdbPath(std::string path);
    static const std::string& adbPath();

    /// True if the configured adb executable can actually be run (i.e. it
    /// exists and is on PATH), independent of whether any device is
    /// connected. Used to give a clear error instead of a confusing
    /// "no devices found" when adb itself is missing.
    static bool isAdbAvailable();

    /// Lists devices currently visible to `adb devices -l`, including
    /// unauthorized/offline ones (check DeviceInfo fields before using).
    static std::vector<DeviceInfo> listConnectedDevices();

    const std::string& serial() const { return serial_; }

    /// True if this serial appears in `adb devices` with state "device".
    bool isConnected() const;

    /// Runs `adb shell <command>`, where `command` is one fully-formed,
    /// already-quoted shell command string.
    ProcessResult shell(const std::string& command) const;

    /// Runs shell(command) and returns trimmed stdout. Sets *ok (if given)
    /// to whether the command exited 0.
    std::string shellText(const std::string& command, bool* ok = nullptr) const;

    bool push(const std::string& localPath, const std::string& remotePath) const;

    /// Copies `remotePath` off the device. When `showProgress` is true adb
    /// writes its own progress display straight to the terminal instead of
    /// being captured, which matters for trees big enough that a silent pull
    /// is indistinguishable from a hang.
    bool pull(const std::string& remotePath, const std::string& localPath, bool showProgress = false) const;

    /// Like pull(), but preserves timestamps and modes (`adb pull -a`) and
    /// reports whether adb logged any per-file errors -- which for a
    /// whole-tree pull normally means "permission denied on part of it"
    /// rather than an outright failure. `errorText` receives adb's stderr.
    bool pullTree(const std::string& remotePath, const std::string& localPath, bool* sawErrors,
                   std::string* errorText) const;

    /// Installs one or more APK paths as a single atomic install session
    /// (base + split APKs). `reinstall` maps to `-r` (keep data if present).
    bool installApks(const std::vector<std::string>& localApkPaths, bool reinstall = true) const;

    /// Streams the stdout produced by `command` on the device straight into
    /// the local file at `localFilePath`, via `adb exec-out`. Used to pull
    /// large/binary data (tar streams) without buffering it in this process.
    bool execOutToFile(const std::string& command, const std::string& localFilePath) const;

    /// Streams the local file at `localFilePath` into `command`'s stdin on
    /// the device, via `adb shell` (which forwards stdin to the remote
    /// command). Used to push large/binary data (tar streams) for restore.
    bool shellFromFile(const std::string& command, const std::string& localFilePath) const;

    /// Wraps the legacy `adb backup` flow. Blocks until the user confirms
    /// (or declines) the backup on the device's lock screen.
    bool backupToFile(const std::string& outputFile, const std::vector<std::string>& packages,
                       bool includeApk, bool includeShared) const;

    /// Wraps the legacy `adb restore` flow. Blocks until the user confirms
    /// the restore on the device.
    bool restoreFromFile(const std::string& inputFile) const;

    /// Probes for root access, preferring an already-root adbd, then a
    /// `su` binary reachable from the shell user.
    RootAccess detectRoot() const;

    DeviceInfo queryDeviceInfo() const;

    /// Enumerates installed packages via `pm list packages -f`. When
    /// `includeSystemApps` is false, only third-party (`-3`) packages are
    /// returned. Each PackageInfo's apkPaths holds just the base APK; call
    /// resolveApkPaths() to fill in split APKs for the ones you need.
    std::vector<PackageInfo> listPackages(bool includeSystemApps) const;

    /// Fills in the complete APK set (base + splits) for `packages`, using a
    /// single on-device shell invocation rather than one `pm path` round trip
    /// per package. Packages the device does not answer for keep whatever
    /// listPackages() already found.
    void resolveApkPaths(std::vector<PackageInfo>& packages) const;

    /// Returns every APK path (base + splits) for one installed package.
    std::vector<std::string> packageApkPaths(const std::string& packageName) const;

    /// Of `packageNames`, returns those reachable through `run-as` -- that is,
    /// the packages built with `android:debuggable="true"`. `run-as` executes
    /// a command as the app's own UID, which is the only way to read
    /// /data/data/<pkg> without root.
    ///
    /// Determined in a single on-device shell pass, so the cost does not grow
    /// with the number of packages.
    std::vector<std::string> packagesSupportingRunAs(const std::vector<std::string>& packageNames) const;

    /// Wraps `command` so it runs as `packageName`'s UID via `run-as`. The
    /// caller is responsible for having checked packagesSupportingRunAs().
    static std::string asPackage(const std::string& packageName, const std::string& command);

private:
    std::vector<std::string> baseArgs() const;

    std::string serial_;
};

} // namespace abp
