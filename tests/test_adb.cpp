#include "abp/AdbClient.h"

#include <string>

#include "FakeAdb.h"
#include "TestFramework.h"

using namespace abp;
using abp::test::FakeAdb;

namespace {

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
    ABP_CHECK_EQ(devices[2].model, "Pixel 6 Pro");
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
    // Resolving N packages must cost one adb call, not one per package.
    FakeAdb fake(R"SH(
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

    ABP_CHECK_EQ(fake.callCount(), 1);
}

ABP_TEST(adb_resolve_apk_paths_handles_an_empty_list) {
    FakeAdb fake("exit 1\n"); // Must not be called at all.
    std::vector<PackageInfo> packages;
    AdbClient("SERIAL").resolveApkPaths(packages);
    ABP_CHECK_EQ(packages.size(), 0u);
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

ABP_TEST(adb_detects_run_as_capable_packages_in_one_call) {
    FakeAdb fake(R"SH(
shift $(( $# - 1 ))
case "$1" in
  *"@@abp-runas:"*)
    # Only the debuggable packages answer.
    echo "@@abp-runas:com.example.debuggable"
    echo "@@abp-runas:com.example.alsodebug"
    exit 0;;
esac
exit 1
)SH");

    auto supported = AdbClient("SERIAL").packagesSupportingRunAs(
        {"com.example.debuggable", "com.example.locked", "com.example.alsodebug"});

    ABP_CHECK_EQ(supported.size(), 2u);
    ABP_CHECK_EQ(supported[0], "com.example.debuggable");
    ABP_CHECK_EQ(supported[1], "com.example.alsodebug");

    ABP_CHECK_EQ(fake.callCount(), 1); // One round trip regardless of package count.
}

ABP_TEST(adb_reports_no_run_as_packages_on_a_locked_down_device) {
    FakeAdb fake("exit 1\n");
    auto supported = AdbClient("SERIAL").packagesSupportingRunAs({"com.example.app"});
    ABP_CHECK_EQ(supported.size(), 0u);
}

ABP_TEST(adb_run_as_probe_ignores_unrequested_and_invalid_names) {
    // A device that echoes back a package nobody asked about must not have it
    // treated as run-as capable.
    FakeAdb fake(R"SH(
shift $(( $# - 1 ))
case "$1" in
  *"@@abp-runas:"*)
    echo "@@abp-runas:com.example.asked"
    echo "@@abp-runas:com.example.never.asked.about"
    exit 0;;
esac
exit 1
)SH");

    auto supported = AdbClient("SERIAL").packagesSupportingRunAs({"com.example.asked"});
    ABP_CHECK_EQ(supported.size(), 1u);
    ABP_CHECK_EQ(supported[0], "com.example.asked");

    // Names that fail validation never reach the device command.
    auto none = AdbClient("SERIAL").packagesSupportingRunAs({"com.example; rm -rf /"});
    ABP_CHECK_EQ(none.size(), 0u);

    ABP_CHECK_EQ(AdbClient("SERIAL").packagesSupportingRunAs({}).size(), 0u);
}

ABP_TEST(adb_as_package_quotes_the_package_name) {
    ABP_CHECK_EQ(AdbClient::asPackage("com.example.app", "tar -czf - ."),
                 "run-as 'com.example.app' tar -czf - .");
}

ABP_TEST(adb_connection_problem_is_empty_for_one_ready_device) {
    FakeAdb fake(kDevicesScript);
    ABP_CHECK_EQ(AdbClient("").connectionProblem(), std::string());
    ABP_CHECK_EQ(AdbClient("GOODDEV").connectionProblem(), std::string());
}

ABP_TEST(adb_refuses_to_guess_between_several_ready_devices) {
    // adb itself rejects every command with "more than one device/emulator"
    // here, so abp must ask for a serial instead of pressing on.
    FakeAdb fake(R"SH(
case "$1" in
  devices)
    echo "List of devices attached"
    echo "PHONE1                 device model:One"
    echo "PHONE2                 device model:Two"
    exit 0;;
esac
exit 1
)SH");

    const std::string problem = AdbClient("").connectionProblem();
    ABP_CHECK(problem.find("--serial") != std::string::npos);
    ABP_CHECK(problem.find("PHONE1") != std::string::npos);
    ABP_CHECK(problem.find("PHONE2") != std::string::npos);
    ABP_CHECK(!AdbClient("").isConnected());
    ABP_CHECK(AdbClient("PHONE2").isConnected());
}

ABP_TEST(adb_connection_problem_explains_unusable_and_missing_devices) {
    FakeAdb fake(R"SH(
case "$1" in
  devices)
    echo "List of devices attached"
    echo "LOCKED                 unauthorized"
    exit 0;;
esac
exit 1
)SH");

    ABP_CHECK(AdbClient("").connectionProblem().find("Allow USB debugging") != std::string::npos);
    ABP_CHECK(AdbClient("LOCKED").connectionProblem().find("unauthorized") != std::string::npos);
    ABP_CHECK(AdbClient("ELSEWHERE").connectionProblem().find("'ELSEWHERE'") != std::string::npos);
}

ABP_TEST(adb_resolve_apk_paths_keeps_answers_when_the_last_package_fails) {
    // The batch's exit status is that of its final `pm path`. A package that
    // vanished mid-run must not discard the answers for all the others.
    FakeAdb fake(R"SH(
case "$*" in
  *"@@abp:"*)
    echo "@@abp:com.example.app"
    echo "package:/data/app/app/base.apk"
    echo "package:/data/app/app/split_config.arm64_v8a.apk"
    echo "@@abp:com.example.gone"
    exit 1;;
esac
exit 1
)SH");

    PackageInfo app;
    app.name = "com.example.app";
    app.apkPaths = {"/data/app/app/base.apk"};
    PackageInfo gone;
    gone.name = "com.example.gone";
    gone.apkPaths = {"/data/app/gone/base.apk"};
    std::vector<PackageInfo> packages{app, gone};

    AdbClient("SERIAL").resolveApkPaths(packages);

    ABP_CHECK_EQ(packages[0].apkPaths.size(), 2u);
    ABP_CHECK_EQ(packages[1].apkPaths.size(), 1u);
    ABP_CHECK_EQ(packages[1].apkPaths[0], "/data/app/gone/base.apk");
}

ABP_TEST(adb_splits_long_package_scripts_across_several_shell_calls) {
    // One shell call per package is too slow, but one call for every package
    // on a device with a few hundred can exceed what adb accepts. The fake
    // answers `pm path` for whatever the script it was handed asks about,
    // rejects any script over 4 KiB, and counts its calls.
    FakeAdb fake(R"SH(
shift $(( $# - 1 ))
script="$1"
if [ ${#script} -gt 4096 ]; then echo "service string too long" >&2; exit 1; fi
printf '%s\n' "$script" | tr ';' '\n' | sed -n "s/^ *echo '@@abp:\(.*\)'.*/\1/p" | while read -r name; do
  echo "@@abp:$name"
  echo "package:/data/app/$name/base.apk"
  echo "package:/data/app/$name/split_extra.apk"
done
exit 0
)SH");

    std::vector<PackageInfo> packages;
    for (int i = 0; i < 150; ++i) {
        PackageInfo pkg;
        pkg.name = "com.example.some.rather.long.package.name.number" + std::to_string(i);
        packages.push_back(pkg);
    }

    AdbClient("SERIAL").resolveApkPaths(packages);

    for (const auto& pkg : packages) {
        ABP_CHECK_EQ(pkg.apkPaths.size(), 2u);
        ABP_CHECK_EQ(pkg.apkPaths[0], "/data/app/" + pkg.name + "/base.apk");
    }

    ABP_CHECK(fake.callCount() > 1);
    ABP_CHECK(fake.callCount() < 20);
}

ABP_TEST(adb_pins_the_one_ready_device_when_no_serial_is_given) {
    // adb counts the offline device too and would refuse an unpinned command,
    // so the client must name the ready one explicitly.
    FakeAdb fake(kDevicesScript);
    ABP_CHECK_EQ(AdbClient("").pinned().serial(), std::string("GOODDEV"));
    ABP_CHECK_EQ(AdbClient("OFFLINEDEV").pinned().serial(), std::string("OFFLINEDEV"));
}

ABP_TEST(adb_finds_mounted_removable_storage) {
    FakeAdb fake(R"SH(
shift $(( $# - 1 ))
case "$1" in
  "sm list-volumes public"*)
    echo "public:179,1 mounted 1A2B-3C4D"
    echo "public:8,1 unmounted 9999-0000"
    echo "public:8,17 mounted not/a/uuid"
    exit 0;;
esac
exit 1
)SH");
    auto roots = AdbClient("SERIAL").removableStorageRoots();
    ABP_CHECK_EQ(roots.size(), 1u);
    ABP_CHECK_EQ(roots[0], "/storage/1A2B-3C4D");
}

ABP_TEST(adb_falls_back_to_listing_storage_without_sm) {
    FakeAdb fake(R"SH(
shift $(( $# - 1 ))
case "$1" in
  "ls /storage"*) printf 'emulated\nself\nABCD-1234\nsdcard0\n'; exit 0;;
esac
exit 1
)SH");
    auto roots = AdbClient("SERIAL").removableStorageRoots();
    ABP_CHECK_EQ(roots.size(), 1u);
    ABP_CHECK_EQ(roots[0], "/storage/ABCD-1234");
}
