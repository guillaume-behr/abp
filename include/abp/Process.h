#pragma once

#include <string>
#include <vector>

namespace abp {

/// Result of running a child process to completion.
struct ProcessResult {
    int exitCode = -1;
    std::string stdOut;
    std::string stdErr;
    /// True if the process could not even be started (e.g. binary not found).
    bool spawnFailed = false;

    bool ok() const { return !spawnFailed && exitCode == 0; }
};

/// Thin, dependency-free wrapper around fork/exec for running external
/// programs (adb, in practice) without going through a shell. Arguments are
/// passed as an argv vector, so there is never any shell interpolation on
/// the host side.
class Process {
public:
    /// Runs `args[0]` with the given arguments, capturing stdout and stderr
    /// into memory. Suitable for short, text-producing commands.
    /// If `stdinData` is non-null, its contents are written to the child's
    /// stdin before it is closed.
    static ProcessResult run(const std::vector<std::string>& args, const std::string* stdinData = nullptr);

    /// Runs `args`, redirecting the child's stdout directly to the file at
    /// `outputPath` (created/truncated). Use this for large/binary output
    /// (e.g. streaming a tar archive off a device) so it never passes
    /// through this process' memory. stderr is still captured as text.
    static ProcessResult runToFile(const std::vector<std::string>& args, const std::string& outputPath);

    /// Runs `args`, redirecting the child's stdin directly from the file at
    /// `inputPath`. Use this to stream large/binary input (e.g. pushing a
    /// tar archive into an on-device restore command) without buffering it.
    static ProcessResult runFromFile(const std::vector<std::string>& args, const std::string& inputPath);

    /// Runs `args` with this process' own stdout and stderr, so the child
    /// writes straight to the terminal. Nothing is captured: the returned
    /// ProcessResult carries only the exit code.
    ///
    /// Use this for bulk transfers (`adb pull` of a large tree), where adb's
    /// own progress display is the only sign of life during an operation that
    /// can run for many minutes. Capturing that output instead would make a
    /// multi-gigabyte pull look like a hang.
    static ProcessResult runInheritStdio(const std::vector<std::string>& args);
};

} // namespace abp
