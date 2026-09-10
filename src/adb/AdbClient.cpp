#include "abp/AdbClient.h"

#include <sstream>

#include "abp/FsUtil.h"
#include "abp/StringUtil.h"

namespace abp {
namespace {

std::string& adbPathStorage() {
    static std::string path = "adb";
    return path;
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

    std::istringstream stream(r.stdOut);
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        if (first) {
            first = false;
            continue; // "List of devices attached"
        }
        std::string trimmed = strutil::trim(line);
        if (trimmed.empty()) continue;

        std::istringstream tokens(trimmed);
        DeviceInfo info;
        tokens >> info.serial >> info.state;

        std::string kv;
        while (tokens >> kv) {
            auto pos = kv.find(':');
            if (pos == std::string::npos) continue;
            std::string key = kv.substr(0, pos);
            std::string value = kv.substr(pos + 1);
            if (key == "model") info.model = value;
            if (key == "product") info.manufacturer = value;
        }
        devices.push_back(std::move(info));
    }
    return devices;
}

bool AdbClient::isConnected() const {
    for (const auto& device : listConnectedDevices()) {
        if (serial_.empty() || device.serial == serial_) {
            return device.isReady();
        }
    }
    return false;
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
    return r.ok() && r.stdOut.find("Success") != std::string::npos;
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

    std::string which = shellText("which su 2>/dev/null || command -v su 2>/dev/null", &ok);
    if (!ok || which.empty()) {
        return access; // RootMethod::None
    }

    bool suOk = false;
    std::string suUid = shellText("su -c id -u 2>/dev/null", &suOk);
    if (suOk && suUid == "0") {
        access.method = RootMethod::SuBinary;
    }
    return access;
}

DeviceInfo AdbClient::queryDeviceInfo() const {
    DeviceInfo info;
    info.serial = serial_;
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

std::vector<std::string> AdbClient::packageApkPaths(const std::string& packageName) const {
    std::vector<std::string> paths;
    if (!strutil::isValidPackageName(packageName)) {
        return paths;
    }

    bool ok = false;
    std::string output = shellText("pm path " + packageName, &ok);
    if (!ok) return paths;

    for (const auto& rawLine : strutil::split(output, '\n')) {
        std::string line = strutil::trim(rawLine);
        if (strutil::startsWith(line, "package:")) {
            paths.push_back(line.substr(std::string("package:").size()));
        }
    }
    return paths;
}

std::vector<PackageInfo> AdbClient::listPackages(bool includeSystemApps) const {
    std::vector<PackageInfo> result;

    std::string cmd = includeSystemApps ? "pm list packages -f" : "pm list packages -f -3";
    bool ok = false;
    std::string output = shellText(cmd, &ok);
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
        info.apkPaths = packageApkPaths(pkgName);
        if (info.apkPaths.empty() && !apkPath.empty()) {
            info.apkPaths.push_back(apkPath);
        }
        info.isSystemApp = strutil::startsWith(apkPath, "/system/") ||
                            strutil::startsWith(apkPath, "/product/") ||
                            strutil::startsWith(apkPath, "/vendor/") ||
                            strutil::startsWith(apkPath, "/apex/");
        result.push_back(std::move(info));
    }
    return result;
}

} // namespace abp
