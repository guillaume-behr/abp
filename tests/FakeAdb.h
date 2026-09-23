#pragma once

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "abp/AdbClient.h"
#include "abp/FsUtil.h"

namespace abp::test {

/// A shell script standing in for `adb`, plus a scratch directory, for tests
/// that exercise device-facing code without a device (or adb) present.
///
/// The script receives adb's arguments verbatim -- `-s SERIAL` first when a
/// serial is set -- and every invocation is appended to calls.log next to it,
/// so tests can assert on what was (or was not) run. AdbClient is pointed at
/// the script for the fixture's lifetime; the previous adb path is restored
/// and the directory removed on destruction.
class FakeAdb {
public:
    explicit FakeAdb(const std::string& script) : previousPath_(AdbClient::adbPath()) {
        static int counter = 0;
        dir_ = std::filesystem::temp_directory_path() /
               ("abp_fake_adb_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(dir_ / "backup");

        const std::filesystem::path adb = dir_ / "adb";
        std::ofstream out(adb);
        out << "#!/bin/sh\necho \"$*\" >> \"$(dirname \"$0\")/calls.log\"\n" << script;
        out.close();
        std::filesystem::permissions(adb, std::filesystem::perms::owner_all);
        AdbClient::setAdbPath(adb.string());
    }

    ~FakeAdb() {
        AdbClient::setAdbPath(previousPath_);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    FakeAdb(const FakeAdb&) = delete;
    FakeAdb& operator=(const FakeAdb&) = delete;

    /// The directory holding the script; scripts can write files there too.
    const std::filesystem::path& dir() const { return dir_; }

    /// An empty directory to use as a backup's root.
    std::filesystem::path backupDir() const { return dir_ / "backup"; }

    /// Every adb invocation so far, one line each.
    std::string readLog() const {
        const std::filesystem::path log = dir_ / "calls.log";
        return std::filesystem::exists(log) ? fsutil::readTextFile(log) : std::string();
    }

    /// Number of adb invocations so far.
    int callCount() const {
        const std::string log = readLog();
        int count = 0;
        for (char c : log) count += c == '\n' ? 1 : 0;
        return count;
    }

private:
    std::string previousPath_;
    std::filesystem::path dir_;
};

/// Script prologue that drops a leading `-s SERIAL`, so a `case "$1"` below
/// sees the adb subcommand whether or not the client pinned a serial.
inline constexpr const char* kSkipSerial = "[ \"$1\" = \"-s\" ] && shift 2\n";

} // namespace abp::test
