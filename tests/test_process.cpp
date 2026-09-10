#include "abp/Process.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "TestFramework.h"

using namespace abp;

ABP_TEST(process_captures_stdout_stderr_and_exit_code) {
    ProcessResult r = Process::run({"sh", "-c", "printf out; printf err >&2; exit 3"});
    ABP_CHECK(!r.spawnFailed);
    ABP_CHECK_EQ(r.exitCode, 3);
    ABP_CHECK_EQ(r.stdOut, "out");
    ABP_CHECK_EQ(r.stdErr, "err");
    ABP_CHECK(!r.ok());
}

ABP_TEST(process_reports_success) {
    ProcessResult r = Process::run({"sh", "-c", "exit 0"});
    ABP_CHECK(r.ok());
    ABP_CHECK_EQ(r.exitCode, 0);
}

ABP_TEST(process_rejects_empty_argv) {
    ProcessResult r = Process::run({});
    ABP_CHECK(r.spawnFailed);
    ABP_CHECK(!r.ok());
}

ABP_TEST(process_reports_missing_binary_without_spawn_failure) {
    // execvp() failing inside the child surfaces as the shell's 127, not as a
    // spawnFailed: the fork itself worked.
    ProcessResult r = Process::run({"abp-no-such-binary-exists-here"});
    ABP_CHECK(!r.ok());
    ABP_CHECK_EQ(r.exitCode, 127);
}

ABP_TEST(process_reports_signal_death_as_128_plus_signo) {
    ProcessResult r = Process::run({"sh", "-c", "kill -TERM $$"});
    ABP_CHECK(!r.ok());
    ABP_CHECK_EQ(r.exitCode, 128 + 15); // SIGTERM
}

ABP_TEST(process_streams_stdin_larger_than_a_pipe_buffer) {
    // Comfortably past the 64 KiB pipe capacity, so the write has to block
    // and resume rather than completing in one go.
    std::string payload(1024 * 1024, 'x');
    ProcessResult r = Process::run({"wc", "-c"}, &payload);
    ABP_CHECK(r.ok());
    ABP_CHECK_EQ(std::stoul(r.stdOut), payload.size());
}

ABP_TEST(process_survives_a_child_that_never_reads_its_stdin) {
    // Writing to a pipe whose reader exited raises SIGPIPE; if that signal is
    // not neutralised it kills the test binary outright rather than failing.
    std::string payload(4 * 1024 * 1024, 'y');
    ProcessResult r = Process::run({"sh", "-c", "exit 0"}, &payload);
    ABP_CHECK(!r.spawnFailed);
}

ABP_TEST(process_does_not_close_unrelated_descriptors) {
    // The stdin pipe used to be closed twice: once by the io pump and again by
    // run(). A second close of a recycled number takes an innocent fd with it.
    int sentinel = ::open("/dev/null", O_RDONLY);
    ABP_CHECK(sentinel >= 0);

    std::string payload = "hello";
    for (int i = 0; i < 20; ++i) {
        Process::run({"cat"}, &payload);
    }

    ABP_CHECK(::fcntl(sentinel, F_GETFD) != -1); // Still open.
    ::close(sentinel);
}

ABP_TEST(process_does_not_leak_descriptors_across_runs) {
    auto openFdCount = [] {
        int count = 0;
        for (int fd = 0; fd < 256; ++fd) {
            if (::fcntl(fd, F_GETFD) != -1) ++count;
        }
        return count;
    };

    Process::run({"sh", "-c", "exit 0"}); // Warm up any lazy allocations.
    const int before = openFdCount();
    for (int i = 0; i < 20; ++i) {
        std::string payload = "data";
        Process::run({"cat"}, &payload);
        Process::run({"sh", "-c", "echo x >&2"});
    }
    ABP_CHECK_EQ(openFdCount(), before);
}

namespace {

/// Path to a scratch file that is removed when the object goes out of scope.
class TempFile {
public:
    explicit TempFile(const std::string& suffix) {
        path_ = "abp_test_" + std::to_string(::getpid()) + "_" + suffix;
    }
    ~TempFile() { ::unlink(path_.c_str()); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

std::string readAll(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return {};
    std::string out;
    char buffer[4096];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        out.append(buffer, n);
    }
    std::fclose(f);
    return out;
}

} // namespace

ABP_TEST(process_run_to_file_writes_stdout_and_captures_stderr) {
    TempFile out("to_file.txt");
    ProcessResult r = Process::runToFile({"sh", "-c", "printf payload; printf oops >&2"}, out.path());
    ABP_CHECK(r.ok());
    ABP_CHECK_EQ(readAll(out.path()), "payload");
    ABP_CHECK_EQ(r.stdErr, "oops");
    // Large output must not also be buffered in memory.
    ABP_CHECK_EQ(r.stdOut, "");
}

ABP_TEST(process_run_to_file_reports_an_unopenable_target) {
    ProcessResult r = Process::runToFile({"sh", "-c", "echo hi"}, "/abp-no-such-dir/out.txt");
    ABP_CHECK(r.spawnFailed);
    ABP_CHECK(!r.ok());
}

ABP_TEST(process_run_from_file_streams_the_file_as_stdin) {
    TempFile in("from_file.txt");
    {
        std::FILE* f = std::fopen(in.path().c_str(), "wb");
        ABP_CHECK(f != nullptr);
        std::string payload(300000, 'z');
        std::fwrite(payload.data(), 1, payload.size(), f);
        std::fclose(f);
    }

    ProcessResult r = Process::runFromFile({"wc", "-c"}, in.path());
    ABP_CHECK(r.ok());
    ABP_CHECK_EQ(std::stoul(r.stdOut), 300000u);
}

ABP_TEST(process_run_from_file_reports_a_missing_source) {
    ProcessResult r = Process::runFromFile({"cat"}, "abp-no-such-input-file");
    ABP_CHECK(r.spawnFailed);
    ABP_CHECK(!r.ok());
}

ABP_TEST(process_redirects_correctly_when_standard_streams_are_closed) {
    // With stdin/stdout/stderr closed, pipe() hands back fds 0/1/2, which the
    // child's redirects would otherwise overwrite before using them. Run the
    // check in a forked child so the test binary keeps its own streams, and
    // report the outcome back through a pipe.
    int report[2];
    ABP_CHECK(::pipe(report) == 0);

    pid_t pid = ::fork();
    ABP_CHECK(pid >= 0);

    if (pid == 0) {
        ::close(STDIN_FILENO);
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);

        ProcessResult r = Process::run({"sh", "-c", "printf hello"});
        const bool good = r.ok() && r.stdOut == "hello";

        char answer = good ? 'y' : 'n';
        ssize_t written = ::write(report[1], &answer, 1);
        ::_exit(written == 1 ? 0 : 1);
    }

    ::close(report[1]);
    char answer = '?';
    ssize_t got = ::read(report[0], &answer, 1);
    ::close(report[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);

    ABP_CHECK_EQ(got, static_cast<ssize_t>(1));
    ABP_CHECK_EQ(answer, 'y');
}
