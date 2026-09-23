#include "abp/Cli.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <unistd.h>

#include "abp/AdbClient.h"
#include "abp/BackupManager.h"
#include "abp/BackupOptions.h"
#include "abp/BackupStore.h"
#include "abp/DevicePaths.h"
#include "abp/GuiServer.h"
#include "abp/Json.h"
#include "abp/Logger.h"
#include "abp/Process.h"
#include "abp/StringUtil.h"
#include "abp/Version.h"

namespace abp {
namespace {

/// First Ctrl-C: stop the backup/restore cleanly (the running adb is
/// terminated, and a backup still writes the manifest for what it captured).
/// SA_RESETHAND puts the default action back, so a second Ctrl-C exits at
/// once for someone who does not want to wait.
extern "C" void onInterrupt(int) {
    static const char kMessage[] = "\nCancelling... (press Ctrl-C again to quit immediately)\n";
    ssize_t ignored = ::write(STDERR_FILENO, kMessage, sizeof(kMessage) - 1);
    (void)ignored;
    Process::requestCancel();
}

void installInterruptHandler() {
    struct sigaction action {};
    action.sa_handler = onInterrupt;
    sigemptyset(&action.sa_mask);
    action.sa_flags = static_cast<int>(SA_RESETHAND);
    ::sigaction(SIGINT, &action, nullptr);
}

const char* kUsage = R"(abp - ADB-based Android backup and restore tool

Usage:
  abp devices
  abp info [-s SERIAL]
  abp list-packages [-s SERIAL] [--system] [--json]
  abp backup -o DIR [options]
  abp restore -i DIR [options]
  abp verify -i DIR
  abp gui [options]
  abp --help
  abp -V | --version

Global options (accepted before or after the subcommand):
      --adb-path PATH     Use this adb executable instead of the one on PATH
                          (or set the ABP_ADB_PATH environment variable).
  -v, --verbose           Print debug-level progress output.
      --no-color          Disable coloured output (also honours NO_COLOR).

Backup options:
  -s, --serial SERIAL     Target a specific device (see 'abp devices').
  -o, --output DIR        Directory to write the backup into (required).
      --system            Include system apps (default: third-party apps only).
      --no-apks           Skip extracting APK files.
      --no-data           Skip app data (root: per-app tar; standard: legacy adb backup).
      --no-shared         Skip /sdcard (shared storage / media) and removable SD cards.
      --no-personal       Skip exporting contacts, SMS/MMS, call log, calendar, settings
                          and (with root) Wi-Fi networks.
      --only PKGS         Comma-separated package names to include, all others excluded.
      --exclude PKGS      Comma-separated package names to exclude.
      --root              Require root; fail if unavailable.
      --standard          Force standard (non-root) mode even if root is available.
      --all-files         Also copy every persistent device partition verbatim
                          with 'adb pull' (/data, /sdcard, /system, /vendor,
                          ...). Without root, most of /data is unreadable and
                          is captured only partially. This can be very large.
      --pull-path PATH    Also copy one device path verbatim with 'adb pull'.
                          Repeatable. Implies the same capture as --all-files
                          but for exactly the paths you name.
  -y, --yes               Do not prompt for confirmation.

GUI options:
      --port PORT         Port to listen on (default: 8787, 0 picks a free one).
      --host ADDR         Address to bind (default: 127.0.0.1, loopback only).
  -d, --backup-dir DIR    Folder the "Explore backups" view starts from
                          (default: the current directory).
      --scan-depth N      How many levels below that folder to search (default: 2).
      --no-browser        Do not open a browser automatically.

Restore options:
  -s, --serial SERIAL     Target a specific device.
  -i, --input DIR         Backup directory to restore from (required).
      --no-apks           Do not reinstall APKs.
      --no-data           Do not restore app data.
      --no-shared         Do not restore shared storage.
      --no-personal       Do not copy contacts/calendar to the device or re-add Wi-Fi networks.
      --only PKGS         Comma-separated package names to restore.
      --exclude PKGS      Comma-separated package names to skip.
  -y, --yes               Do not prompt for confirmation.

Root mode requires either a device with an already-root adbd (e.g. a
userdebug build, or after 'adb root') or a working 'su' binary reachable
from the adb shell user (Magisk, etc). Standard mode uses only public adb
functionality and works on any device with USB debugging enabled, but has
real limitations -- see docs/ROOT_BACKUP.md and README.md.
)";

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> result;
    for (const auto& part : strutil::split(value, ',')) {
        std::string trimmed = strutil::trim(part);
        if (!trimmed.empty()) result.push_back(trimmed);
    }
    return result;
}

bool confirm(const std::string& prompt) {
    std::cout << prompt << " [y/N] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    line = strutil::trim(line);
    return line == "y" || line == "Y" || line == "yes" || line == "Yes";
}

bool ensureAdbAvailable() {
    if (AdbClient::isAdbAvailable()) return true;
    Logger::error("Could not run '" + AdbClient::adbPath() +
                  "'. Install Android platform-tools and make sure adb is on your PATH.");
    return false;
}

/// The value following the option at args[i], advancing i past it. Throws
/// (reported as a usage error by Cli::run) when the option is last.
const std::string& optionValue(const std::vector<std::string>& args, size_t& i) {
    if (i + 1 >= args.size()) throw std::runtime_error(args[i] + " requires a value");
    return args[++i];
}

int cmdDevices() {
    if (!ensureAdbAvailable()) return 1;
    auto devices = AdbClient::listConnectedDevices();
    if (devices.empty()) {
        std::cout << "No devices found. Is USB debugging enabled and the device connected/authorized?\n";
        return 0;
    }
    for (const auto& d : devices) {
        std::cout << d.serial << "\t" << d.state;
        if (!d.model.empty()) std::cout << "\t" << d.model;
        std::cout << "\n";
    }
    return 0;
}

int cmdInfo(const std::string& serial) {
    if (!ensureAdbAvailable()) return 1;
    AdbClient adb(serial);
    if (std::string problem = adb.connectionProblem(); !problem.empty()) {
        Logger::error(problem);
        return 1;
    }
    adb = adb.pinned();
    DeviceInfo info = adb.queryDeviceInfo();
    std::cout << "Serial:        " << info.serial << "\n";
    std::cout << "Manufacturer:  " << info.manufacturer << "\n";
    std::cout << "Model:         " << info.model << "\n";
    std::cout << "Android:       " << info.androidRelease << " (SDK " << info.sdkInt << ")\n";
    std::cout << "Root access:   " << (info.isRooted() ? "yes" : "no") << " (" << describeRootMethod(info.root.method)
               << ")\n";
    return 0;
}

int cmdListPackages(const std::string& serial, bool includeSystem, bool asJson) {
    if (!ensureAdbAvailable()) return 1;
    AdbClient adb(serial);
    if (std::string problem = adb.connectionProblem(); !problem.empty()) {
        Logger::error(problem);
        return 1;
    }
    adb = adb.pinned();
    auto packages = adb.listPackages(includeSystem);
    adb.resolveApkPaths(packages); // Fill in split APKs so the counts below are real.

    if (asJson) {
        json::JsonValue arr = json::JsonValue::makeArray();
        for (const auto& pkg : packages) {
            json::JsonValue obj = json::JsonValue::makeObject();
            obj.set("name", pkg.name);
            obj.set("system_app", pkg.isSystemApp);
            obj.set("apk_count", static_cast<int>(pkg.apkPaths.size()));
            arr.push_back(obj);
        }
        std::cout << arr.dump(2) << "\n";
        return 0;
    }

    for (const auto& pkg : packages) {
        std::cout << pkg.name;
        if (pkg.isSystemApp) std::cout << "\t[system]";
        if (pkg.apkPaths.size() > 1) std::cout << "\t(" << pkg.apkPaths.size() << " apk files)";
        std::cout << "\n";
    }
    std::cout << packages.size() << " package(s).\n";
    return 0;
}

int cmdBackup(const std::vector<std::string>& args) {
    BackupOptions options;
    bool haveOutput = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "-s" || arg == "--serial") options.serial = optionValue(args, i);
        else if (arg == "-o" || arg == "--output") { options.outputDir = optionValue(args, i); haveOutput = true; }
        else if (arg == "--system") options.includeSystemApps = true;
        else if (arg == "--no-apks") options.includeApks = false;
        else if (arg == "--no-data") options.includeAppData = false;
        else if (arg == "--no-shared") options.includeSharedStorage = false;
        else if (arg == "--no-personal") options.exportPersonalData = false;
        else if (arg == "--only") options.onlyPackages = splitCsv(optionValue(args, i));
        else if (arg == "--exclude") options.excludePackages = splitCsv(optionValue(args, i));
        else if (arg == "--root") options.mode = BackupMode::Root;
        else if (arg == "--standard") options.mode = BackupMode::Standard;
        else if (arg == "--all-files") {
            for (const auto& root : devicepaths::defaultCaptureRoots()) {
                options.filesystemPaths.push_back(root);
            }
        }
        else if (arg == "--pull-path") options.filesystemPaths.push_back(optionValue(args, i));
        else if (arg == "-y" || arg == "--yes") options.assumeYes = true;
        else {
            Logger::error("Unknown backup option: " + arg);
            return 2;
        }
    }

    if (!haveOutput) {
        Logger::error("backup requires -o/--output DIR");
        return 2;
    }

    // Reject an unusable --pull-path now, rather than after a long backup has
    // already run. --all-files supplies its own paths, so this only ever
    // rejects something the user typed.
    for (const auto& path : options.filesystemPaths) {
        const devicepaths::PathVerdict verdict = devicepaths::classify(path);
        if (verdict != devicepaths::PathVerdict::Ok) {
            Logger::error(devicepaths::explainVerdict(verdict, path));
            return 2;
        }
    }

    if (!ensureAdbAvailable()) return 1;

    if (!options.assumeYes) {
        std::cout << "About to back up device" << (options.serial.empty() ? "" : " " + options.serial) << " into '"
                  << options.outputDir.string() << "'.\n";
        if (!options.filesystemPaths.empty()) {
            std::cout << "This includes a verbatim 'adb pull' of: "
                      << strutil::join(devicepaths::collapseRedundant(options.filesystemPaths), ", ") << "\n"
                      << "Copying whole partitions can take a long time and produce many gigabytes.\n";
        }
        if (!confirm("Continue?")) {
            std::cout << "Aborted.\n";
            return 1;
        }
    }

    installInterruptHandler();
    BackupSummary summary = BackupManager::runBackup(options);
    for (const auto& message : summary.messages) Logger::error(message);

    if (!summary.success) return 1;

    std::cout << "\nBackup complete (" << summary.mode << " mode).\n";
    std::cout << "  Packages:        " << summary.packageCount << "\n";
    std::cout << "  With app data:   " << summary.packagesWithData << "\n";

    // Spell out how that data was actually obtained. In standard mode the
    // split between `run-as` and the legacy archive is the difference between
    // a complete per-app capture and a best-effort one, so it is worth saying.
    if (summary.packagesCapturedByRootTar > 0) {
        std::cout << "    via root tar:   " << summary.packagesCapturedByRootTar << "\n";
    }
    if (summary.packagesCapturedByRunAs > 0) {
        std::cout << "    via run-as:     " << summary.packagesCapturedByRunAs
                   << " (complete per-app archives)\n";
    }
    if (summary.packagesCapturedByLegacyBackup > 0) {
        std::cout << "    via adb backup: " << summary.packagesCapturedByLegacyBackup
                   << " (partial; apps may have opted out)\n";
    }

    std::cout << "  Errors:          " << summary.packagesWithErrors << "\n";
    std::cout << "  Shared storage:  " << (summary.sharedStorageIncluded ? "included" : "skipped") << "\n";
    if (summary.removableStorageCount > 0) {
        std::cout << "  SD cards:        " << summary.removableStorageCount << " copied\n";
    }
    for (const auto& item : summary.personalExports) {
        std::string label = item.label + ":";
        label.resize(17, ' ');
        std::cout << "  " << label
                  << (item.count < 0 ? "not exported (" + item.reason + ")" : std::to_string(item.count)) << "\n";
    }
    if (summary.filesystemCaptureCount > 0) {
        std::cout << "  Device paths:    " << summary.filesystemCaptureCount << " pulled";
        if (summary.filesystemPartialCount > 0) {
            std::cout << " (" << summary.filesystemPartialCount << " only partially readable)";
        }
        std::cout << "\n";
    }
    std::cout << "  Total size:      " << strutil::formatBytes(summary.totalBytes) << "\n";
    std::cout << "  Output:          " << summary.outputDir.string() << "\n";
    if (!summary.warnings.empty()) {
        std::cout << "\nBefore relying on this backup:\n";
        for (const auto& warning : summary.warnings) std::cout << "  - " << warning << "\n";
    }
    return 0;
}

int cmdRestore(const std::vector<std::string>& args) {
    RestoreOptions options;
    bool haveInput = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "-s" || arg == "--serial") options.serial = optionValue(args, i);
        else if (arg == "-i" || arg == "--input") { options.inputDir = optionValue(args, i); haveInput = true; }
        else if (arg == "--no-apks") options.includeApks = false;
        else if (arg == "--no-data") options.includeAppData = false;
        else if (arg == "--no-shared") options.includeSharedStorage = false;
        else if (arg == "--no-personal") options.includePersonalData = false;
        else if (arg == "--only") options.onlyPackages = splitCsv(optionValue(args, i));
        else if (arg == "--exclude") options.excludePackages = splitCsv(optionValue(args, i));
        else if (arg == "-y" || arg == "--yes") options.assumeYes = true;
        else {
            Logger::error("Unknown restore option: " + arg);
            return 2;
        }
    }

    if (!haveInput) {
        Logger::error("restore requires -i/--input DIR");
        return 2;
    }

    if (!ensureAdbAvailable()) return 1;

    if (!options.assumeYes) {
        std::cout << "About to restore '" << options.inputDir.string() << "' onto device"
                  << (options.serial.empty() ? "" : " " + options.serial)
                  << ".\nThis will overwrite existing app data for restored packages.\n";
        if (!confirm("Continue?")) {
            std::cout << "Aborted.\n";
            return 1;
        }
    }

    installInterruptHandler();
    RestoreSummary summary = BackupManager::runRestore(options);
    for (const auto& message : summary.messages) Logger::error(message);

    if (!summary.success) return 1;

    std::cout << "\nRestore complete.\n";
    std::cout << "  Packages restored: " << summary.packagesRestored << "\n";
    std::cout << "  Packages failed:   " << summary.packagesFailed << "\n";
    if (summary.packagesSkipped > 0) {
        std::cout << "  Nothing to restore: " << summary.packagesSkipped
                   << " (no APK and no data in the backup)\n";
    }
    std::cout << "  Shared storage:    " << (summary.sharedStorageRestored ? "restored" : "skipped") << "\n";
    if (!summary.personal.contactsPath.empty()) {
        std::cout << "  Contacts:          copied to " << summary.personal.contactsPath << " (import it in Contacts)\n";
    }
    if (!summary.personal.calendarPath.empty()) {
        std::cout << "  Calendar:          copied to " << summary.personal.calendarPath << " (open it to import)\n";
    }
    if (summary.personal.wifiRestored >= 0) {
        std::cout << "  Wi-Fi networks:    " << summary.personal.wifiRestored << " re-added";
        if (summary.personal.wifiSkipped > 0) std::cout << ", " << summary.personal.wifiSkipped << " skipped";
        std::cout << "\n";
    }
    if (summary.filesystemCapturesPresent > 0) {
        std::cout << "  Device paths:      " << summary.filesystemCapturesPresent
                   << " present, not restored (copy by hand)\n";
    }
    return summary.packagesFailed > 0 ? 1 : 0;
}

int cmdVerify(const std::vector<std::string>& args) {
    std::string input;
    for (size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "-i" || args[i] == "--input") && i + 1 < args.size()) {
            input = args[++i];
        } else if (input.empty() && !args[i].empty() && args[i][0] != '-') {
            input = args[i];
        } else {
            Logger::error("Unknown verify option: " + args[i]);
            return 2;
        }
    }
    if (input.empty()) {
        Logger::error("verify requires -i/--input DIR");
        return 2;
    }

    const VerifyReport report = BackupStore::verify(input);
    std::cout << "Checked '" << input << "':\n";
    std::cout << "  Checksums matched:   " << report.verified << "\n";
    std::cout << "  Present, no checksum: " << report.unverifiable << "\n";
    if (report.ok()) {
        std::cout << "  Problems:            none\n";
        return 0;
    }
    std::cout << "  Problems:            " << report.problems.size() << "\n";
    for (const auto& problem : report.problems) std::cout << "    " << problem.path << ": " << problem.problem << "\n";
    return 1;
}

int parseIntOption(const std::string& flag, const std::string& value) {
    try {
        size_t consumed = 0;
        int parsed = std::stoi(value, &consumed);
        if (consumed == value.size()) return parsed;
    } catch (const std::exception&) {
        // Fall through to the shared error below.
    }
    throw std::runtime_error(flag + " expects a number, got '" + value + "'");
}

int cmdGui(const std::vector<std::string>& args) {
    GuiOptions options;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "--port") options.port = parseIntOption(arg, optionValue(args, i));
        else if (arg == "--host") options.host = optionValue(args, i);
        else if (arg == "-d" || arg == "--backup-dir") options.backupRoot = optionValue(args, i);
        else if (arg == "--scan-depth") options.scanDepth = parseIntOption(arg, optionValue(args, i));
        else if (arg == "--no-browser") options.openBrowser = false;
        else {
            Logger::error("Unknown gui option: " + arg);
            return 2;
        }
    }

    if (options.port < 0 || options.port > 65535) {
        Logger::error("--port must be between 0 and 65535.");
        return 2;
    }
    if (options.scanDepth < 0) {
        Logger::error("--scan-depth cannot be negative.");
        return 2;
    }

    if (const char* envToken = std::getenv("ABP_GUI_TOKEN")) options.token = envToken;

    if (!AdbClient::isAdbAvailable()) {
        // Not fatal: exploring existing backups needs no device at all, and
        // adb may well appear before the user clicks "Back up".
        Logger::warn("Could not run '" + AdbClient::adbPath() +
                     "'. The GUI will start, but backing up and restoring needs adb on your PATH.");
    }

    return GuiServer::run(options);
}

} // namespace

int Cli::run(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (const char* envAdbPath = std::getenv("ABP_ADB_PATH")) {
        AdbClient::setAdbPath(envAdbPath);
    }
    // https://no-color.org: any non-empty value disables colour.
    if (const char* noColor = std::getenv("NO_COLOR")) {
        if (noColor[0] != '\0') Logger::setColorEnabled(false);
    }

    // Pull the global options out of argv wherever they appear, so each
    // subcommand's own parser only ever sees its own flags.
    for (size_t i = 0; i < args.size();) {
        if (args[i] == "--adb-path") {
            if (i + 1 >= args.size()) {
                Logger::error("--adb-path requires a value");
                return 2;
            }
            AdbClient::setAdbPath(args[i + 1]);
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
        } else if (args[i] == "--no-color") {
            Logger::setColorEnabled(false);
            args.erase(args.begin() + static_cast<long>(i));
        } else if (args[i] == "-v" || args[i] == "--verbose") {
            Logger::setVerbose(true);
            args.erase(args.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }

    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        // With no arguments at all this is a usage error, so it goes to stderr
        // and exits non-zero; an explicit `--help` is a successful request.
        std::ostream& out = args.empty() ? std::cerr : std::cout;
        out << kUsage;
        return args.empty() ? 1 : 0;
    }

    if (args[0] == "-V" || args[0] == "--version" || args[0] == "version") {
        std::cout << "abp " << kVersionString << "\n";
        return 0;
    }

    try {
        const std::string& command = args[0];
        std::vector<std::string> rest(args.begin() + 1, args.end());

        if (command == "devices") {
            for (const auto& a : rest) {
                // `-l` is accepted and ignored: abp always asks adb for the
                // long listing, and it is what adb users reach for by habit.
                if (a == "-l") continue;
                Logger::error("Unknown devices option: " + a);
                return 2;
            }
            return cmdDevices();
        }

        // Only the two read-only commands below take their serial this way;
        // backup/restore/gui parse their own arguments, and scanning theirs
        // here would answer an unknown flag with the wrong complaint.
        const bool takesSerial = command == "info" || command == "list-packages";
        std::string serial;
        for (size_t i = 0; takesSerial && i < rest.size(); ++i) {
            if (rest[i] != "-s" && rest[i] != "--serial") continue;
            if (i + 1 >= rest.size()) {
                Logger::error(rest[i] + " requires a value");
                return 2;
            }
            serial = rest[i + 1];
        }

        if (command == "info") {
            for (size_t i = 0; i < rest.size(); ++i) {
                if (rest[i] == "-s" || rest[i] == "--serial") {
                    ++i; // Value already collected above.
                    continue;
                }
                Logger::error("Unknown info option: " + rest[i]);
                return 2;
            }
            return cmdInfo(serial);
        }

        if (command == "list-packages") {
            bool includeSystem = false;
            bool asJson = false;
            for (size_t i = 0; i < rest.size(); ++i) {
                const std::string& a = rest[i];
                if (a == "--system") includeSystem = true;
                else if (a == "--json") asJson = true;
                else if (a == "-s" || a == "--serial") ++i; // Value already collected above.
                else {
                    Logger::error("Unknown list-packages option: " + a);
                    return 2;
                }
            }
            return cmdListPackages(serial, includeSystem, asJson);
        }

        if (command == "backup") return cmdBackup(rest);
        if (command == "restore") return cmdRestore(rest);
        if (command == "verify") return cmdVerify(rest);
        if (command == "gui") return cmdGui(rest);

        Logger::error("Unknown command: " + command);
        std::cerr << kUsage;
        return 2;
    } catch (const std::exception& e) {
        Logger::error(e.what());
        return 2;
    }
}

} // namespace abp
