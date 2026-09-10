#include "abp/AdbClient.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "TestFramework.h"

using namespace abp;
namespace fs = std::filesystem;

namespace {

/// Writes an executable stand-in for `adb` and points AdbClient at it, so the
/// parsing logic can be exercised without a device (or adb) present. The
/// script and the previous adb path are both restored on destruction.
class FakeAdb {
public:
    explicit FakeAdb(const std::string& script) : previousPath_(AdbClient::adbPath()) {
        dir_ = fs::temp_directory_path() / ("abp_fake_adb_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(counter()++));
        fs::create_directories(dir_);
        path_ = dir_ / "adb";

        std::ofstream out(path_);
        out << "#!/bin/sh\n" << script;
        out.close();
        fs::permissions(path_, fs::perms::owner_all);

        AdbClient::setAdbPath(path_.string());
    }

    ~FakeAdb() {
        AdbClient::setAdbPath(previousPath_);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    FakeAdb(const FakeAdb&) = delete;
    FakeAdb& operator=(const FakeAdb&) = delete;

private:
    static int& counter() {
        static int value = 0;
        return value;
    }

    std::string previousPath_;
    fs::path dir_;
    fs::path path_;
};

/// `adb devices -l` output as it really arrives when the daemon has to start:
/// banner lines on stdout ahead of the header, and an unusable device listed
/// before the usable one.
const char* kDevicesScript = R"SH(
case "$1" in
  version) echo "Android Debug Bridge version 1.0.41"; exit 0;;
  devices)
    echo "* daemon not running; starting now at tcp:5037"
    echo "* daemon started successfully"
    echo "List of devices attached"
    echo "OFFLINEDEV             offline"
    echo "NOPERMDEV              no permissions; see [http://example.invalid]"
    echo "GOODDEV                device product:raven model:Pixel_6_Pro device:raven transport_id:2"
    exit 0;;
esac
exit 1
)SH";

} // namespace

ABP_TEST(adb_ignores_daemon_banner_lines_when_listing_devices) {
    FakeAdb fake(kDevicesScript);

    auto devices = AdbClient::listConnectedDevices();
    ABP_CHECK_EQ(devices.size(), 3u); // Not the banner lines, not the header.
    ABP_CHECK_EQ(devices[0].serial, "OFFLINEDEV");
    ABP_CHECK_EQ(devices[0].state, "offline");
    ABP_CHECK_EQ(devices[1].serial, "NOPERMDEV");
    ABP_CHECK_EQ(devices[1].state, "no permissions");
    ABP_CHECK_EQ(devices[2].serial, "GOODDEV");
    ABP_CHECK_EQ(devices[2].state, "device");
    ABP_CHECK_EQ(devices[2].model, "Pixel_6_Pro");
}

ABP_TEST(adb_finds_a_ready_device_past_unusable_ones) {
    FakeAdb fake(kDevicesScript);

    // With no serial pinned, an offline device listed first must not decide
    // the answer for the ready device behind it.
    ABP_CHECK(AdbClient("").isConnected());
    ABP_CHECK(AdbClient("GOODDEV").isConnected());
    ABP_CHECK(!AdbClient("OFFLINEDEV").isConnected());
    ABP_CHECK(!AdbClient("NOPERMDEV").isConnected());
    ABP_CHECK(!AdbClient("NOSUCHDEV").isConnected());
}

ABP_TEST(adb_reports_no_connection_when_only_unusable_devices_are_present) {
    FakeAdb fake(R"SH(
case "$1" in
  version) exit 0;;
  devices)
    echo "List of devices attached"
    echo "OFFLINEDEV             offline"
    exit 0;;
esac
exit 1
)SH");

    ABP_CHECK(!AdbClient("").isConnected());
}

ABP_TEST(adb_availability_follows_the_configured_binary) {
    {
        FakeAdb fake("exit 0\n");
        ABP_CHECK(AdbClient::isAdbAvailable());
    }
    {
        FakeAdb fake("exit 1\n");
        ABP_CHECK(!AdbClient::isAdbAvailable());
    }
}

ABP_TEST(adb_lists_packages_and_flags_system_apps) {
    FakeAdb fake(R"SH(
while [ "$1" = "-s" ] || [ "$1" = "$ABP_SERIAL" ]; do shift; done
case "$*" in
  *"pm list packages"*)
    echo "package:/data/app/~~a==/com.example.app-b==/base.apk=com.example.app"
    echo "package:/system/priv-app/Settings/Settings.apk=com.android.settings"
    echo "not a package line"
    echo "package:/data/app/x.apk=not a valid package name"
    exit 0;;
esac
exit 1
)SH");

    auto packages = AdbClient("SERIAL").listPackages(true);
    ABP_CHECK_EQ(packages.size(), 2u); // Junk and invalid names are dropped.
    ABP_CHECK_EQ(packages[0].name, "com.example.app");
    ABP_CHECK(!packages[0].isSystemApp);
    ABP_CHECK_EQ(packages[1].name, "com.android.settings");
    ABP_CHECK(packages[1].isSystemApp);
}

ABP_TEST(adb_resolves_split_apks_in_a_single_shell_call) {
    // The script counts its own invocations; resolving N packages must cost
    // one adb call, not one per package.
    FakeAdb fake(R"SH(
echo x >> "${TMPDIR:-/tmp}/abp_shell_calls"
case "$*" in
  *"@@abp:"*)
    echo "@@abp:com.example.app"
    echo "package:/data/app/~~a==/com.example.app-b==/base.apk"
    echo "package:/data/app/~~a==/com.example.app-b==/split_config.xxhdpi.apk"
    echo "@@abp:com.example.solo"
    echo "package:/data/app/~~c==/com.example.solo-d==/base.apk"
    exit 0;;
esac
exit 1
)SH");

    const std::string counter =
        (fs::temp_directory_path() / "abp_shell_calls").string();
    std::error_code ec;
    fs::remove(counter, ec);

    std::vector<PackageInfo> packages;
    PackageInfo a;
    a.name = "com.example.app";
    a.apkPaths = {"/data/app/~~a==/com.example.app-b==/base.apk"};
    PackageInfo b;
    b.name = "com.example.solo";
    b.apkPaths = {"/data/app/~~c==/com.example.solo-d==/base.apk"};
    PackageInfo missing;
    missing.name = "com.example.unanswered";
    missing.apkPaths = {"/data/app/keep-me.apk"};
    packages = {a, b, missing};

    AdbClient("SERIAL").resolveApkPaths(packages);

    ABP_CHECK_EQ(packages[0].apkPaths.size(), 2u);
    ABP_CHECK_EQ(packages[0].apkPaths[1], "/data/app/~~a==/com.example.app-b==/split_config.xxhdpi.apk");
    ABP_CHECK_EQ(packages[1].apkPaths.size(), 1u);
    // A package the device did not answer for keeps what it already had.
    ABP_CHECK_EQ(packages[2].apkPaths.size(), 1u);
    ABP_CHECK_EQ(packages[2].apkPaths[0], "/data/app/keep-me.apk");

    std::ifstream calls(counter);
    std::string line;
    int callCount = 0;
    while (std::getline(calls, line)) ++callCount;
    calls.close();
    fs::remove(counter, ec);
    ABP_CHECK_EQ(callCount, 1);
}

ABP_TEST(adb_resolve_apk_paths_handles_an_empty_list) {
    FakeAdb fake("exit 1\n"); // Must not be called at all.
    std::vector<PackageInfo> packages;
    AdbClient("SERIAL").resolveApkPaths(packages);
    ABP_CHECK_EQ(packages.size(), 0u);
}

ABP_TEST(adb_package_apk_paths_returns_the_full_set_for_one_package) {
    FakeAdb fake(R"SH(
shift $(( $# - 1 ))
case "$1" in
  "pm path com.example.app")
    echo "package:/data/app/~~a==/com.example.app-b==/base.apk"
    echo "package:/data/app/~~a==/com.example.app-b==/split_config.en.apk"
    exit 0;;
esac
exit 1
)SH");

    AdbClient adb("SERIAL");
    auto paths = adb.packageApkPaths("com.example.app");
    ABP_CHECK_EQ(paths.size(), 2u);
    ABP_CHECK_EQ(paths[0], "/data/app/~~a==/com.example.app-b==/base.apk");
    ABP_CHECK_EQ(paths[1], "/data/app/~~a==/com.example.app-b==/split_config.en.apk");

    // A package the device does not know about yields nothing, not an error.
    ABP_CHECK_EQ(adb.packageApkPaths("com.example.absent").size(), 0u);
}

ABP_TEST(adb_package_apk_paths_rejects_an_invalid_package_name) {
    // A name that could carry shell metacharacters must never reach the
    // device command at all.
    FakeAdb fake("echo 'package:/should/not/happen.apk'\nexit 0\n");
    AdbClient adb("SERIAL");
    ABP_CHECK_EQ(adb.packageApkPaths("com.example; rm -rf /").size(), 0u);
    ABP_CHECK_EQ(adb.packageApkPaths("").size(), 0u);
}

ABP_TEST(adb_detects_root_via_an_already_root_shell) {
    FakeAdb fake(R"SH(
case "$*" in
  *"id -u"*) echo 0; exit 0;;
esac
exit 1
)SH");

    RootAccess access = AdbClient("SERIAL").detectRoot();
    ABP_CHECK(access.available());
    ABP_CHECK(access.method == RootMethod::AdbdRoot);
}

ABP_TEST(adb_detects_root_via_a_quoted_su_command) {
    // A su that only accepts its command as a single quoted argument -- the
    // unquoted `su -c id -u` form fails here, as it does on real devices.
    FakeAdb fake(R"SH(
shift $(( $# - 1 ))
cmd="$1"
case "$cmd" in
  "id -u") echo 2000; exit 0;;
  *"command -v su"*) echo /system/bin/su; exit 0;;
  "su -c 'id -u' 2>/dev/null") echo 0; exit 0;;
esac
exit 1
)SH");

    RootAccess access = AdbClient("SERIAL").detectRoot();
    ABP_CHECK(access.available());
    ABP_CHECK(access.method == RootMethod::SuBinary);
}

ABP_TEST(adb_reports_no_root_when_su_is_absent) {
    FakeAdb fake(R"SH(
shift $(( $# - 1 ))
case "$1" in
  "id -u") echo 2000; exit 0;;
esac
exit 1
)SH");

    RootAccess access = AdbClient("SERIAL").detectRoot();
    ABP_CHECK(!access.available());
    ABP_CHECK(access.method == RootMethod::None);
}
