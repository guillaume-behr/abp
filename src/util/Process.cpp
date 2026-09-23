#include "abp/Process.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <system_error>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace abp {
namespace {

/// Lock-free so requestCancel() stays async-signal-safe (the CLI calls it
/// from its SIGINT handler).
std::atomic<bool> g_cancelRequested{false};

/// How long a child gets to exit after SIGTERM before it is killed outright.
constexpr auto kTerminateGrace = std::chrono::seconds(2);

/// Sends SIGTERM on the first call after a cancel request, and SIGKILL once
/// the grace period has passed. Returns true once a signal has been sent.
class CancelWatch {
public:
    explicit CancelWatch(pid_t pid) : pid_(pid) {}

    void check() {
        if (!g_cancelRequested.load()) return;
        const auto now = std::chrono::steady_clock::now();
        if (!terminated_) {
            ::kill(pid_, SIGTERM);
            terminated_ = true;
            terminatedAt_ = now;
        } else if (!killed_ && now - terminatedAt_ > kTerminateGrace) {
            ::kill(pid_, SIGKILL);
            killed_ = true;
        }
    }

    bool fired() const { return terminated_; }

private:
    pid_t pid_;
    bool terminated_ = false;
    bool killed_ = false;
    std::chrono::steady_clock::time_point terminatedAt_{};
};

/// Result for a run*() call made while a cancel is pending.
ProcessResult cancelledResult() {
    ProcessResult result;
    result.spawnFailed = true;
    result.cancelled = true;
    result.stdErr = "cancelled";
    return result;
}

/// RAII wrapper around a file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { close(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int release() {
        int f = fd_;
        fd_ = -1;
        return f;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

/// RAII pair of pipe fds. Both ends are closed on destruction unless they
/// were released or closed explicitly, so every early return below (a failed
/// second pipe(), a failed fork()) unwinds without leaking descriptors.
class Pipe {
public:
    /// Both ends are close-on-exec. abp forks from more than one thread (the
    /// GUI serves requests while a backup runs), and without this a fork
    /// happening concurrently on another thread would inherit this pipe --
    /// holding its write end open and stalling the unrelated reader until
    /// that second child also exited. dup2() clears the flag on whatever the
    /// child installs as stdin/stdout/stderr, so those still survive the exec.
    bool open() {
        int fds[2] = {-1, -1};
#if defined(__linux__)
        if (::pipe2(fds, O_CLOEXEC) != 0) return false;
#else
        if (::pipe(fds) != 0) return false;
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
        readEnd_ = Fd(fds[0]);
        writeEnd_ = Fd(fds[1]);
        return true;
    }

    Fd& read() { return readEnd_; }
    Fd& write() { return writeEnd_; }

private:
    Fd readEnd_;
    Fd writeEnd_;
};

/// Writing to a pipe whose reader has gone away raises SIGPIPE, whose
/// default action is to kill the process. abp would rather see write() fail
/// with EPIPE and report it, so the signal is ignored process-wide the first
/// time any child is spawned.
void ignoreSigPipeOnce() {
    static const bool ignored = [] {
        struct sigaction sa {};
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        // Only install the handler if the process has not already chosen its
        // own disposition for SIGPIPE (a library embedding abp_core might).
        struct sigaction previous {};
        if (sigaction(SIGPIPE, nullptr, &previous) == 0 && previous.sa_handler == SIG_DFL) {
            sigaction(SIGPIPE, &sa, nullptr);
        }
        return true;
    }();
    (void)ignored;
}

/// In the child: points `target` (STDIN_FILENO/STDOUT_FILENO/...) at `source`
/// and then drops `source`. Guards the case where `source` already occupies
/// the target slot -- closing it there would leave the child with that
/// standard stream closed instead of redirected.
void redirect(Fd& source, int target) {
    if (source.get() == target) {
        // Already in place; must not be closed. It does still need the
        // close-on-exec flag cleared by hand: the pipe was created with it,
        // and dup2 -- which drops the flag everywhere else in this function --
        // is not involved on this path, so the exec would close the stream.
        int flags = fcntl(target, F_GETFD);
        if (flags != -1) fcntl(target, F_SETFD, flags & ~FD_CLOEXEC);
        source.release();
        return;
    }

    // If `source` currently sits in one of the standard slots, a later
    // redirect() could overwrite it before it has been used -- pipe fds only
    // land down there when abp was started with a standard stream closed.
    // Move it out of the way first so the redirects cannot clobber each other.
    if (source.get() >= 0 && source.get() < 3) {
        int relocated = fcntl(source.get(), F_DUPFD, 3);
        if (relocated >= 0) {
            source.close();
            source = Fd(relocated);
        }
    }

    dup2(source.get(), target);
    source.close();
}

/// Opens /dev/null for reading, close-on-exec, as the stdin of a child that
/// is given no input. Inheriting abp's own stdin instead would let `adb shell`
/// (which forwards its stdin to the device) swallow whatever the user types,
/// and when abp runs as a background job -- `abp gui &` -- the child's first
/// read of the terminal stops it with SIGTTIN, hanging abp along with it.
/// Returns an invalid Fd if /dev/null cannot be opened; the child then
/// inherits stdin as before, which is no worse than not trying.
Fd openNullInput() { return Fd(::open("/dev/null", O_RDONLY | O_CLOEXEC)); }

/// Builds the argv array exec() needs. This MUST be called before fork(),
/// never in the child: it allocates, and the only async-signal-safe thing a
/// forked child of a multi-threaded process may do is exec. abp forks from
/// more than one thread (the GUI serves requests while a backup runs), so a
/// child that allocated could deadlock forever on a malloc lock another
/// thread happened to hold at the moment of the fork. The strings it points
/// into belong to the caller's `args` and survive the fork unchanged.
std::vector<char*> buildArgv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        // POSIX guarantees exec() implementations do not modify argv strings.
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/// Shuttles data between the parent and a child process: writes `stdinData`
/// (if any) to `stdinFd`, and reads from `stdoutFd`/`stderrFd` into the
/// result until both are closed by the child. Any of the three fds may be
/// invalid (-1), meaning that stream is not piped.
///
/// `stdinPipe` is closed here as soon as the data is written (that is what
/// tells the child its input has ended), so it is taken by reference and the
/// close is recorded in the Fd itself rather than leaving the caller holding
/// a descriptor number that has already been handed back to the kernel.
void pumpIo(Fd& stdinPipe, int stdoutFd, int stderrFd, const std::string* stdinData, ProcessResult& result,
            CancelWatch& cancel) {
    const int stdinFd = stdinPipe.get();
    if (stdinFd >= 0) setNonBlocking(stdinFd);
    if (stdoutFd >= 0) setNonBlocking(stdoutFd);
    if (stderrFd >= 0) setNonBlocking(stderrFd);

    size_t stdinOffset = 0;
    bool stdinOpen = stdinFd >= 0;
    bool stdoutOpen = stdoutFd >= 0;
    bool stderrOpen = stderrFd >= 0;

    std::array<char, 65536> buffer{};

    while (stdinOpen || stdoutOpen || stderrOpen) {
        std::vector<pollfd> fds;
        int stdinIdx = -1, stdoutIdx = -1, stderrIdx = -1;

        if (stdinOpen) {
            stdinIdx = static_cast<int>(fds.size());
            fds.push_back({stdinFd, POLLOUT, 0});
        }
        if (stdoutOpen) {
            stdoutIdx = static_cast<int>(fds.size());
            fds.push_back({stdoutFd, POLLIN, 0});
        }
        if (stderrOpen) {
            stderrIdx = static_cast<int>(fds.size());
            fds.push_back({stderrFd, POLLIN, 0});
        }

        // A bounded wait, so a cancel request is noticed even while the child
        // is silent (a long `adb pull` writes nothing we capture).
        int rc = poll(fds.data(), static_cast<nfds_t>(fds.size()), 200);
        cancel.check();
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        const short stdinEvents =
            stdinIdx >= 0 ? fds[static_cast<size_t>(stdinIdx)].revents : static_cast<short>(0);
        if (stdinEvents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) {
            const bool nothingLeft = stdinData == nullptr || stdinOffset >= stdinData->size();
            if (nothingLeft || !(stdinEvents & POLLOUT)) {
                // Either the child has everything we owe it, or the pipe has
                // errored/hung up. Closing our end signals end-of-input.
                stdinPipe.close();
                stdinOpen = false;
            } else {
                ssize_t written = ::write(stdinFd, stdinData->data() + stdinOffset, stdinData->size() - stdinOffset);
                if (written > 0) {
                    stdinOffset += static_cast<size_t>(written);
                } else if (written < 0 && errno != EAGAIN && errno != EINTR) {
                    // EPIPE lands here rather than killing the process,
                    // because SIGPIPE is ignored (see ignoreSigPipeOnce).
                    stdinPipe.close();
                    stdinOpen = false;
                }
            }
        }

        const short stdoutEvents = stdoutIdx >= 0 ? fds[static_cast<size_t>(stdoutIdx)].revents : static_cast<short>(0);
        if (stdoutEvents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
            ssize_t n = ::read(stdoutFd, buffer.data(), buffer.size());
            if (n > 0) {
                result.stdOut.append(buffer.data(), static_cast<size_t>(n));
            } else if (n == 0) {
                stdoutOpen = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                stdoutOpen = false;
            } else if (stdoutEvents & (POLLERR | POLLHUP | POLLNVAL)) {
                // Hung up with nothing left to read. Without this the next
                // poll() would return the same flags immediately, forever.
                stdoutOpen = false;
            }
        }

        const short stderrEvents = stderrIdx >= 0 ? fds[static_cast<size_t>(stderrIdx)].revents : static_cast<short>(0);
        if (stderrEvents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
            ssize_t n = ::read(stderrFd, buffer.data(), buffer.size());
            if (n > 0) {
                result.stdErr.append(buffer.data(), static_cast<size_t>(n));
            } else if (n == 0) {
                stderrOpen = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                stderrOpen = false;
            } else if (stderrEvents & (POLLERR | POLLHUP | POLLNVAL)) {
                // Hung up with nothing left to read. Without this the next
                // poll() would return the same flags immediately, forever.
                stderrOpen = false;
            }
        }
    }
}

/// Reaps `pid` and translates its wait status into a ProcessResult exit code.
/// A child killed by a signal is reported as 128+signo, matching the
/// convention shells use, so callers can tell "adb died" from "adb exited 1".
void reap(pid_t pid, ProcessResult& result, CancelWatch& cancel) {
    // Polled rather than a blocking waitpid() so a cancel request can reach
    // a child that is still running. The pause starts at 1 ms and backs off,
    // so the many short adb calls (which have usually exited by the time
    // their pipes close) pay almost nothing for it.
    int status = 0;
    long pauseMs = 1;
    while (true) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) {
            result.exitCode = -1;
            result.cancelled = cancel.fired();
            return;
        }
        cancel.check();
        timespec pause{0, pauseMs * 1000 * 1000};
        nanosleep(&pause, nullptr);
        pauseMs = pauseMs < 50 ? pauseMs * 2 : 50;
    }
    result.cancelled = cancel.fired();
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = 128 + WTERMSIG(status);
    } else {
        result.exitCode = -1;
    }
}

} // namespace

ProcessResult Process::run(const std::vector<std::string>& args, const std::string* stdinData) {
    ProcessResult result;
    if (args.empty()) {
        result.spawnFailed = true;
        return result;
    }
    if (g_cancelRequested.load()) return cancelledResult();
    ignoreSigPipeOnce();

    Pipe inPipe;
    Pipe outPipe;
    Pipe errPipe;

    if (stdinData != nullptr && !inPipe.open()) {
        result.spawnFailed = true;
        return result;
    }
    if (!outPipe.open() || !errPipe.open()) {
        result.spawnFailed = true;
        return result;
    }
    Fd nullInput = stdinData == nullptr ? openNullInput() : Fd();

    auto argv = buildArgv(args);

    pid_t pid = fork();
    if (pid < 0) {
        result.spawnFailed = true;
        return result;
    }

    if (pid == 0) {
        // Child.
        inPipe.write().close();
        outPipe.read().close();
        errPipe.read().close();
        if (stdinData != nullptr) {
            redirect(inPipe.read(), STDIN_FILENO);
        } else if (nullInput.valid()) {
            redirect(nullInput, STDIN_FILENO);
        }
        redirect(outPipe.write(), STDOUT_FILENO);
        redirect(errPipe.write(), STDERR_FILENO);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    // Parent: drop the ends owned by the child so the pipes report EOF.
    nullInput.close();
    inPipe.read().close();
    outPipe.write().close();
    errPipe.write().close();

    CancelWatch cancel(pid);
    pumpIo(inPipe.write(), outPipe.read().get(), errPipe.read().get(), stdinData, result, cancel);

    reap(pid, result, cancel);
    return result;
}

ProcessResult Process::runToFile(const std::vector<std::string>& args, const std::string& outputPath) {
    ProcessResult result;
    if (args.empty()) {
        result.spawnFailed = true;
        return result;
    }
    if (g_cancelRequested.load()) return cancelledResult();
    ignoreSigPipeOnce();

    Fd outFile(open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (!outFile.valid()) {
        result.spawnFailed = true;
        result.stdErr = std::string("failed to open output file: ") + std::generic_category().message(errno);
        return result;
    }

    Pipe errPipe;
    if (!errPipe.open()) {
        result.spawnFailed = true;
        return result;
    }
    Fd nullInput = openNullInput();

    auto argv = buildArgv(args);

    pid_t pid = fork();
    if (pid < 0) {
        result.spawnFailed = true;
        return result;
    }

    if (pid == 0) {
        errPipe.read().close();
        if (nullInput.valid()) redirect(nullInput, STDIN_FILENO);
        redirect(outFile, STDOUT_FILENO);
        redirect(errPipe.write(), STDERR_FILENO);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    nullInput.close();
    outFile.close();
    errPipe.write().close();

    Fd noStdin;
    CancelWatch cancel(pid);
    pumpIo(noStdin, -1, errPipe.read().get(), nullptr, result, cancel);

    reap(pid, result, cancel);
    return result;
}

ProcessResult Process::runFromFile(const std::vector<std::string>& args, const std::string& inputPath) {
    ProcessResult result;
    if (args.empty()) {
        result.spawnFailed = true;
        return result;
    }
    if (g_cancelRequested.load()) return cancelledResult();
    ignoreSigPipeOnce();

    Fd inFile(open(inputPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (!inFile.valid()) {
        result.spawnFailed = true;
        result.stdErr = std::string("failed to open input file: ") + std::generic_category().message(errno);
        return result;
    }

    Pipe outPipe;
    Pipe errPipe;
    if (!outPipe.open() || !errPipe.open()) {
        result.spawnFailed = true;
        return result;
    }

    auto argv = buildArgv(args);

    pid_t pid = fork();
    if (pid < 0) {
        result.spawnFailed = true;
        return result;
    }

    if (pid == 0) {
        outPipe.read().close();
        errPipe.read().close();
        redirect(inFile, STDIN_FILENO);
        redirect(outPipe.write(), STDOUT_FILENO);
        redirect(errPipe.write(), STDERR_FILENO);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    inFile.close();
    outPipe.write().close();
    errPipe.write().close();

    Fd noStdin;
    CancelWatch cancel(pid);
    pumpIo(noStdin, outPipe.read().get(), errPipe.read().get(), nullptr, result, cancel);

    reap(pid, result, cancel);
    return result;
}

void Process::requestCancel() { g_cancelRequested.store(true); }
void Process::clearCancel() { g_cancelRequested.store(false); }
bool Process::cancelRequested() { return g_cancelRequested.load(); }

} // namespace abp
