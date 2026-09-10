#pragma once

#include <string>

namespace abp {

enum class LogLevel { Debug, Info, Warn, Error };

/// Minimal process-wide logger. Info goes to stdout, Warn/Error to stderr.
/// Not thread-safe by design: abp runs its work on a single thread.
class Logger {
public:
    static void setVerbose(bool verbose);

    /// Forces colour on or off. Left alone, each stream is decided
    /// independently by whether it is a terminal, so redirecting only one of
    /// stdout/stderr does not put escape codes into the redirected one.
    static void setColorEnabled(bool enabled);

    static void debug(const std::string& message);
    static void info(const std::string& message);
    static void warn(const std::string& message);
    static void error(const std::string& message);

private:
    static void log(LogLevel level, const std::string& message);
};

} // namespace abp
