#include "abp/Process.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace abp {
namespace {

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

/// Creates a pipe whose ends are close-on-exec. abp now forks from more
/// than one thread (the GUI serves requests while a backup runs), and
/// without this a fork happening concurrently on another thread would
/// inherit this pipe -- holding its write end open and stalling the
/// unrelated reader until that second child also exited.
///
/// dup2() clears the flag on the descriptors the child installs as
/// stdin/stdout/stderr, so those still survive the exec.
bool makeCloexecPipe(int fds[2]) {
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
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
void pumpIo(int stdinFd, int stdoutFd, int stderrFd, const std::string* stdinData, ProcessResult& result) {
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

        int rc = poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (stdinIdx >= 0 && (fds[static_cast<size_t>(stdinIdx)].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (stdinData == nullptr || stdinOffset >= stdinData->size()) {
                ::close(stdinFd);
                stdinOpen = false;
            } else if (fds[static_cast<size_t>(stdinIdx)].revents & POLLOUT) {
                ssize_t written = ::write(stdinFd, stdinData->data() + stdinOffset, stdinData->size() - stdinOffset);
                if (written > 0) {
                    stdinOffset += static_cast<size_t>(written);
                } else if (written < 0 && errno != EAGAIN && errno != EINTR) {
                    ::close(stdinFd);
                    stdinOpen = false;
                }
            } else {
                ::close(stdinFd);
                stdinOpen = false;
            }
        }

        if (stdoutIdx >= 0 && (fds[static_cast<size_t>(stdoutIdx)].revents & (POLLIN | POLLERR | POLLHUP))) {
            ssize_t n = ::read(stdoutFd, buffer.data(), buffer.size());
            if (n > 0) {
                result.stdOut.append(buffer.data(), static_cast<size_t>(n));
            } else if (n == 0) {
                stdoutOpen = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                stdoutOpen = false;
            }
        }

        if (stderrIdx >= 0 && (fds[static_cast<size_t>(stderrIdx)].revents & (POLLIN | POLLERR | POLLHUP))) {
            ssize_t n = ::read(stderrFd, buffer.data(), buffer.size());
            if (n > 0) {
                result.stdErr.append(buffer.data(), static_cast<size_t>(n));
            } else if (n == 0) {
                stderrOpen = false;
            } else if (errno != EAGAIN && errno != EINTR) {
                stderrOpen = false;
            }
        }
    }
}

} // namespace

ProcessResult Process::run(const std::vector<std::string>& args, const std::string* stdinData) {
    ProcessResult result;
    if (args.empty()) {
        result.spawnFailed = true;
        return result;
    }

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};

    if (stdinData != nullptr && !makeCloexecPipe(inPipe)) {
        result.spawnFailed = true;
        return result;
    }
    if (!makeCloexecPipe(outPipe) || !makeCloexecPipe(errPipe)) {
        result.spawnFailed = true;
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.spawnFailed = true;
        return result;
    }

    if (pid == 0) {
        // Child.
        if (stdinData != nullptr) {
            dup2(inPipe[0], STDIN_FILENO);
            close(inPipe[0]);
            close(inPipe[1]);
        }
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);

        auto argv = buildArgv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    // Parent.
    if (stdinData != nullptr) close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);

    pumpIo(stdinData != nullptr ? inPipe[1] : -1, outPipe[0], errPipe[0], stdinData, result);

    if (stdinData != nullptr && inPipe[1] >= 0) close(inPipe[1]);
    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

ProcessResult Process::runToFile(const std::vector<std::string>& args, const std::string& outputPath) {
    ProcessResult result;
    if (args.empty()) {
        result.spawnFailed = true;
        return result;
    }

    Fd outFile(open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (!outFile.valid()) {
        result.spawnFailed = true;
        result.stdErr = std::string("failed to open output file: ") + std::strerror(errno);
        return result;
    }

    int errPipe[2];
    if (!makeCloexecPipe(errPipe)) {
        result.spawnFailed = true;
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.spawnFailed = true;
        return result;
    }

    if (pid == 0) {
        dup2(outFile.get(), STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(errPipe[0]);
        close(errPipe[1]);

        auto argv = buildArgv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    outFile.close();
    close(errPipe[1]);
    pumpIo(-1, -1, errPipe[0], nullptr, result);
    close(errPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

ProcessResult Process::runFromFile(const std::vector<std::string>& args, const std::string& inputPath) {
    ProcessResult result;
    if (args.empty()) {
        result.spawnFailed = true;
        return result;
    }

    Fd inFile(open(inputPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (!inFile.valid()) {
        result.spawnFailed = true;
        result.stdErr = std::string("failed to open input file: ") + std::strerror(errno);
        return result;
    }

    int outPipe[2];
    int errPipe[2];
    if (!makeCloexecPipe(outPipe) || !makeCloexecPipe(errPipe)) {
        result.spawnFailed = true;
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.spawnFailed = true;
        return result;
    }

    if (pid == 0) {
        dup2(inFile.get(), STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);

        auto argv = buildArgv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    inFile.close();
    close(outPipe[1]);
    close(errPipe[1]);
    pumpIo(-1, outPipe[0], errPipe[0], nullptr, result);
    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

} // namespace abp
