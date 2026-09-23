#include "abp/AdbClient.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include "abp/FsUtil.h"
#include "abp/StringUtil.h"

namespace abp {
namespace {

std::string& adbPathStorage() {
    static std::string path = "adb";
    return path;
}

/// Upper bound on one batched `adb shell` command string. Well under the
/// 4 KiB payload limit of pre-Android-7 adbd, leaving room for the service
/// prefix adb adds in front of the command.
constexpr size_t kMaxShellScriptBytes = 3500;

/// What to tell the user about a device adb lists but cannot use yet.
std::string explainDeviceState(const DeviceInfo& device) {
    const std::string& serial = device.serial;
    if (device.state == "unauthorized") {
        return "Device " + serial + " is unauthorized: unlock it and accept the 'Allow USB debugging?' prompt, "
               "then try again.";
    }
    if (device.state == "offline") {
        return "Device " + serial + " is offline: reconnect the USB cable (or run 'adb reconnect') and try again.";
    }
    if (device.state == "no permissions") {
        return "adb has no permission to open device " + serial +
               " (on Linux this usually means a missing udev rule for it).";
    }
    if (device.state == "authorizing" || device.state == "connecting") {
        return "Device " + serial + " is still " + device.state + "; wait a moment and try again.";
    }
    return "Device " + serial + " is in '" + device.state + "' state, not booted into Android with USB debugging.";
}

} // namespace

AdbClient::AdbClient(std::string serial) : serial_(std::move(serial)) {}

void AdbClient::setAdbPath(std::string path) { adbPathStorage() = std::move(path); }
const std::string& AdbClient::adbPath() { return adbPathStorage(); }

bool AdbClient::isAdbAvailable() {
    ProcessResult r = Process::run({adbPath(), "version"});
    return !r.spawnFailed && r.exitCode == 0;
}

std::vector<std::string> AdbClient::baseArgs() const {
    std::vector<std::string> args{adbPath()};
    if (!serial_.empty()) {
        args.push_back("-s");
        args.push_back(serial_);
    }
    return args;
}

std::vector<DeviceInfo> AdbClient::listConnectedDevices() {
    std::vector<DeviceInfo> devices;
    ProcessResult r = Process::run({adbPath(), "devices", "-l"});
    if (r.spawnFailed) {
        return devices;
    }

    // adb may print daemon chatter ("* daemon not running; starting now ...")
    // before the header, so entries are recognised by shape -- exactly two or
    // more whitespace-separated fields whose second field is a known adb
    // connection state -- rather than by "everything after the first line".
    auto isDeviceState = [](const std::string& state) {
        return state == "device" || state == "offline" || state == "unauthorized" ||
               state == "bootloader" || state == "recovery" || state == "sideload" ||
               state == "rescue" || state == "connecting" || state == "authorizing" ||
               state == "host" || state == "no";  // "no permissions; see <url>"
    };

    std::istringstream stream(r.stdOut);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmed = strutil::trim(line);
        if (trimmed.empty()) continue;
        if (strutil::startsWith(trimmed, "*")) continue;                     // daemon chatter
        if (strutil::startsWith(trimmed, "List of devices")) continue;       // header
        if (strutil::startsWith(trimmed, "adb:")) continue;                  // adb error text

        std::istringstream tokens(trimmed);
        DeviceInfo info;
        if (!(tokens >> info.serial >> info.state)) continue;
        if (!isDeviceState(info.state)) continue;
        if (info.state == "no") info.state = "no permissions";

        std::string kv;
        while (tokens >> kv) {
            auto pos = kv.find(':');
            if (pos == std::string::npos) continue;
            std::string key = kv.substr(0, pos);
            std::string value = kv.substr(pos + 1);
            // Only `model` maps onto a DeviceInfo field here. `adb devices -l`
            // reports product/device/transport_id, none of which is the
            // manufacturer -- that comes from getprop in queryDeviceInfo().
            // adb replaces spaces with underscores in this listing.
            if (key == "model") {
                for (char& c : value) {
                    if (c == '_') c = ' ';
                }
                info.model = value;
            }
        }
        devices.push_back(std::move(info));
    }
    return devices;
}

bool AdbClient::isConnected() const { return connectionProblem().empty(); }

std::string AdbClient::connectionProblem() const {
    const std::vector<DeviceInfo> devices = listConnectedDevices();

    if (!serial_.empty()) {
        for (const auto& device : devices) {
            if (device.serial != serial_) continue;
            return device.isReady() ? std::string() : explainDeviceState(device);
        }
        return "No device with serial '" + serial_ + "' is connected. Run 'abp devices' to see what adb can reach.";
    }

    // No serial pinned: with several ready devices there is no telling which
    // one the user meant, and adb itself refuses every unpinned command
    // ("more than one device/emulator"), so that is caught here rather than
    // surfacing as a string of odd failures. Unusable entries are not
    // counted: callers go on to pin the one ready device (see pinned()).
    std::vector<std::string> ready;
    for (const auto& device : devices) {
        if (device.isReady()) ready.push_back(device.serial);
    }
    if (ready.size() == 1) return std::string();
    if (ready.size() > 1) {
        return std::to_string(ready.size()) + " devices are connected (" + strutil::join(ready, ", ") +
               "); choose one with -s/--serial.";
    }
    if (!devices.empty()) return explainDeviceState(devices.front());
    return "No device found. Connect it over USB, enable USB debugging in Developer options, and accept the "
           "'Allow USB debugging?' prompt.";
}

AdbClient AdbClient::pinned() const {
    if (!serial_.empty()) return *this;
    for (const auto& device : listConnectedDevices()) {
        if (device.isReady()) return AdbClient(device.serial);
    }
    return *this;
}

ProcessResult AdbClient::shell(const std::string& command) const {
    auto args = baseArgs();
    args.push_back("shell");
    args.push_back(command);
    return Process::run(args);
}

std::string AdbClient::shellText(const std::string& command, bool* ok) const {
    ProcessResult r = shell(command);
    if (ok) *ok = r.ok();
    return strutil::trim(r.stdOut);
}

bool AdbClient::push(const std::string& localPath, const std::string& remotePath) const {
    auto args = baseArgs();
    args.push_back("push");
    args.push_back(localPath);
    args.push_back(remotePath);
    return Process::run(args).ok();
}

bool AdbClient::pull(const std::string& remotePath, const std::string& localPath) const {
    auto args = baseArgs();
    args.push_back("pull");
    args.push_back(remotePath);
    args.push_back(localPath);
    return Process::run(args).ok();
}

bool AdbClient::pullTree(const std::string& remotePath, const std::string& localPath, bool* sawErrors,
                          std::string* errorText) const {
    auto args = baseArgs();
    args.push_back("pull");
    args.push_back("-a"); // Preserve mtime and mode.
    args.push_back(remotePath);
    args.push_back(localPath);

    ProcessResult r = Process::run(args);
    if (errorText != nullptr) *errorText = strutil::trim(r.stdErr);

    // adb keeps going past a file it cannot read and still exits 0, so the
    // exit code alone would call a half-copied /data a clean capture.
    if (sawErrors != nullptr) {
        const std::string& err = r.stdErr;
        *sawErrors = err.find("Permission denied") != std::string::npos ||
                     err.find("permission denied") != std::string::npos ||
                     err.find("failed to copy") != std::string::npos ||
                     err.find("couldn't read") != std::string::npos ||
                     err.find("skipping") != std::string::npos;
    }
    return r.ok();
}

bool AdbClient::installApks(const std::vector<std::string>& localApkPaths, bool reinstall) const {
    if (localApkPaths.empty()) return false;

    auto args = baseArgs();
    if (localApkPaths.size() == 1) {
        args.push_back("install");
        if (reinstall) args.push_back("-r");
        args.push_back(localApkPaths.front());
    } else {
        args.push_back("install-multiple");
        if (reinstall) args.push_back("-r");
        for (const auto& p : localApkPaths) args.push_back(p);
    }

    ProcessResult r = Process::run(args);
    if (!r.ok()) return false;
    // Which stream carries "Success" varies between adb releases, and some
    // report failure only in the text while still exiting 0.
    const bool sawSuccess = r.stdOut.find("Success") != std::string::npos ||
                            r.stdErr.find("Success") != std::string::npos;
    const bool sawFailure = r.stdOut.find("Failure") != std::string::npos ||
                            r.stdErr.find("Failure") != std::string::npos ||
                            r.stdOut.find("Error:") != std::string::npos ||
                            r.stdErr.find("Error:") != std::string::npos;
    return sawSuccess && !sawFailure;
}

bool AdbClient::execOutToFile(const std::string& command, const std::string& localFilePath) const {
    auto args = baseArgs();
    args.push_back("exec-out");
    args.push_back(command);
    return Process::runToFile(args, localFilePath).ok();
}

bool AdbClient::shellFromFile(const std::string& command, const std::string& localFilePath) const {
    auto args = baseArgs();
    args.push_back("shell");
    args.push_back(command);
    return Process::runFromFile(args, localFilePath).ok();
}

bool AdbClient::backupToFile(const std::string& outputFile, const std::vector<std::string>& packages,
                              bool includeApk, bool includeShared) const {
    auto args = baseArgs();
    args.push_back("backup");
    args.push_back("-f");
    args.push_back(outputFile);
    args.push_back(includeApk ? "-apk" : "-noapk");
    args.push_back(includeShared ? "-shared" : "-noshared");

    if (!packages.empty()) {
        for (const auto& pkg : packages) args.push_back(pkg);
    } else {
        args.push_back("-all");
        args.push_back("-nosystem");
    }

    ProcessResult r = Process::run(args);
    return r.ok() && fsutil::fileSize(outputFile) > 0;
}

bool AdbClient::restoreFromFile(const std::string& inputFile) const {
    auto args = baseArgs();
    args.push_back("restore");
    args.push_back(inputFile);
    return Process::run(args).ok();
}

RootAccess AdbClient::detectRoot() const {
    RootAccess access;

    bool ok = false;
    std::string uid = shellText("id -u", &ok);
    if (ok && uid == "0") {
        access.method = RootMethod::AdbdRoot;
        return access;
    }

    std::string which = shellText("command -v su 2>/dev/null || which su 2>/dev/null", &ok);
    if (!ok || which.empty()) {
        return access; // RootMethod::None
    }

    // `su -c id -u` is ambiguous: several su implementations treat only the
    // first word as the command and pass "-u" to su itself. Quoting keeps the
    // whole thing together as one command for every implementation.
    bool suOk = false;
    std::string suUid = shellText("su -c " + strutil::shellQuote("id -u") + " 2>/dev/null", &suOk);
    if (suOk && strutil::trim(suUid) == "0") {
        access.method = RootMethod::SuBinary;
    }
    return access;
}

DeviceInfo AdbClient::queryDeviceInfo() const {
    DeviceInfo info;
    info.serial = serial_;

    // Fill in the serial and state from `adb devices` when the caller did not
    // pin one, so the manifest records which device was actually captured.
    for (const auto& listed : listConnectedDevices()) {
        if (serial_.empty() ? listed.isReady() : listed.serial == serial_) {
            info.serial = listed.serial;
            info.state = listed.state;
            break;
        }
    }

    info.model = shellText("getprop ro.product.model");
    info.manufacturer = shellText("getprop ro.product.manufacturer");
    info.androidRelease = shellText("getprop ro.build.version.release");

    std::string sdk = shellText("getprop ro.build.version.sdk");
    try {
        info.sdkInt = sdk.empty() ? 0 : std::stoi(sdk);
    } catch (const std::exception&) {
        info.sdkInt = 0;
    }

    info.root = detectRoot();
    return info;
}

std::vector<PackageInfo> AdbClient::listPackages(bool includeSystemApps) const {
    std::vector<PackageInfo> result;

    const std::string listCmd = includeSystemApps ? "pm list packages -f" : "pm list packages -f -3";
    bool ok = false;
    std::string output = shellText(listCmd, &ok);
    if (!ok) return result;

    for (const auto& rawLine : strutil::split(output, '\n')) {
        std::string line = strutil::trim(rawLine);
        if (!strutil::startsWith(line, "package:")) continue;

        std::string rest = line.substr(std::string("package:").size());
        size_t eq = rest.rfind('=');
        if (eq == std::string::npos) continue;

        std::string apkPath = rest.substr(0, eq);
        std::string pkgName = rest.substr(eq + 1);
        if (!strutil::isValidPackageName(pkgName)) continue;

        PackageInfo info;
        info.name = pkgName;
        // The base APK reported by `pm list packages -f`. Split APKs are
        // filled in by resolveApkPaths(), which the caller runs once for the
        // packages it actually cares about.
        if (!apkPath.empty()) info.apkPaths.push_back(apkPath);
        info.isSystemApp = strutil::startsWith(apkPath, "/system/") ||
                            strutil::startsWith(apkPath, "/product/") ||
                            strutil::startsWith(apkPath, "/vendor/") ||
                            strutil::startsWith(apkPath, "/apex/") ||
                            strutil::startsWith(apkPath, "/system_ext/");
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<std::string> AdbClient::removableStorageRoots() const {
    std::vector<std::string> roots;
    auto add = [&roots](const std::string& uuid) {
        // Volume UUIDs are short hex/dash strings; anything else is not a
        // name `sm` or vold would give a public volume.
        if (uuid.empty() || uuid.size() > 36) return;
        for (char c : uuid) {
            if (!std::isxdigit(static_cast<unsigned char>(c)) && c != '-') return;
        }
        const std::string path = "/storage/" + uuid;
        if (std::find(roots.begin(), roots.end(), path) == roots.end()) roots.push_back(path);
    };

    // "public:179,1 mounted 1A2B-3C4D" -- only mounted volumes have a path.
    bool ok = false;
    const std::string volumes = shellText("sm list-volumes public 2>/dev/null", &ok);
    if (ok) {
        for (const auto& rawLine : strutil::split(volumes, '\n')) {
            std::istringstream fields(strutil::trim(rawLine));
            std::string id, state, uuid;
            if (fields >> id >> state >> uuid && strutil::startsWith(id, "public:") && state == "mounted") add(uuid);
        }
        return roots;
    }

    // No `sm` (Android 5 and older): FAT/exFAT volumes mount as XXXX-XXXX.
    const std::string listing = shellText("ls /storage 2>/dev/null", &ok);
    for (const auto& rawLine : strutil::split(listing, '\n')) {
        const std::string name = strutil::trim(rawLine);
        if (name.size() == 9 && name[4] == '-') add(name);
    }
    return roots;
}

std::string AdbClient::asPackage(const std::string& packageName, const std::string& command) {
    return "run-as " + strutil::shellQuote(packageName) + " " + command;
}

std::string AdbClient::runShellBatches(const std::vector<std::string>& snippets) const {
    std::string output;
    std::string script;
    auto flush = [&]() {
        if (script.empty()) return;
        // A batch's exit status is that of its last command, which says
        // nothing about the ones before it, so its output is kept either way.
        output += shell(script).stdOut;
        if (!output.empty() && output.back() != '\n') output.push_back('\n');
        script.clear();
    };

    for (const auto& snippet : snippets) {
        if (!script.empty() && script.size() + snippet.size() > kMaxShellScriptBytes) flush();
        script += snippet;
    }
    flush();
    return output;
}

std::vector<std::string> AdbClient::packagesSupportingRunAs(const std::vector<std::string>& packageNames) const {
    std::vector<std::string> supported;
    if (packageNames.empty()) return supported;

    // `run-as <pkg> id -u` succeeds only for a debuggable package that is
    // installed for the current user. Probing them one at a time would cost an
    // adb round trip each, so the probe runs as on-device scripts that print a
    // marker line per package that answered.
    static const char* kMarker = "@@abp-runas:";

    std::vector<std::string> snippets;
    snippets.reserve(packageNames.size());
    for (const auto& name : packageNames) {
        if (!strutil::isValidPackageName(name)) continue;
        const std::string quoted = strutil::shellQuote(name);
        snippets.push_back("run-as " + quoted + " id -u >/dev/null 2>&1 && echo " +
                           strutil::shellQuote(std::string(kMarker) + name) + "; ");
    }
    if (snippets.empty()) return supported;

    const std::string output = runShellBatches(snippets);

    for (const auto& rawLine : strutil::split(output, '\n')) {
        const std::string line = strutil::trim(rawLine);
        if (!strutil::startsWith(line, kMarker)) continue;
        std::string name = line.substr(std::strlen(kMarker));
        // Only report back packages the caller actually asked about.
        for (const auto& requested : packageNames) {
            if (requested == name) {
                supported.push_back(std::move(name));
                break;
            }
        }
    }
    return supported;
}

void AdbClient::resolveApkPaths(std::vector<PackageInfo>& packages) const {
    if (packages.empty()) return;

    // Asking `pm path` per package costs one adb round trip each, which is
    // tens of seconds on a device with a few hundred apps. Instead run the
    // loop in as few on-device shells as possible, delimiting each package's
    // output with a marker line.
    static const char* kMarker = "@@abp:";

    std::vector<std::string> snippets;
    snippets.reserve(packages.size());
    for (const auto& pkg : packages) {
        if (!strutil::isValidPackageName(pkg.name)) continue;
        snippets.push_back("echo " + strutil::shellQuote(std::string(kMarker) + pkg.name) + "; pm path " +
                           strutil::shellQuote(pkg.name) + " 2>/dev/null; ");
    }
    if (snippets.empty()) return;

    // Parsed whatever the exit status: it is that of the last `pm path`, so a
    // single package uninstalled mid-run would otherwise throw away the
    // answers for every package before it.
    const std::string output = runShellBatches(snippets);

    // Index the reply by package name, then apply it. A package the device
    // did not answer for keeps the base path found by listPackages().
    std::string current;
    std::vector<std::pair<std::string, std::vector<std::string>>> found;
    for (const auto& rawLine : strutil::split(output, '\n')) {
        std::string line = strutil::trim(rawLine);
        if (strutil::startsWith(line, kMarker)) {
            current = line.substr(std::strlen(kMarker));
            found.emplace_back(current, std::vector<std::string>{});
        } else if (!found.empty() && strutil::startsWith(line, "package:")) {
            found.back().second.push_back(line.substr(std::string("package:").size()));
        }
    }

    for (auto& pkg : packages) {
        for (const auto& entry : found) {
            if (entry.first == pkg.name && !entry.second.empty()) {
                pkg.apkPaths = entry.second;
                break;
            }
        }
    }
}

} // namespace abp
