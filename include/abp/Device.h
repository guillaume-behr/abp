#pragma once

#include <string>

namespace abp {

enum class RootMethod {
    None,       ///< No root access available.
    SuBinary,   ///< A `su` binary is available; commands must be run as `su -c '...'`.
    AdbdRoot,   ///< adbd itself is already running as root (`adb root` / userdebug builds).
};

struct RootAccess {
    RootMethod method = RootMethod::None;

    bool available() const { return method != RootMethod::None; }
};

/// Snapshot of the connected device's identity, used to populate the
/// backup manifest and to make backend decisions (e.g. root availability).
struct DeviceInfo {
    std::string serial;
    std::string state = "device"; ///< Raw `adb devices` state: device/offline/unauthorized/....
    std::string model;
    std::string manufacturer;
    std::string androidRelease; ///< e.g. "14"
    int sdkInt = 0;
    RootAccess root;

    bool isRooted() const { return root.available(); }
    bool isReady() const { return state == "device"; }
};

} // namespace abp
