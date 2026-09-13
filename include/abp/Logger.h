#pragma once

#include <functional>
#include <string>

namespace abp {

enum class LogLevel { Debug, Info, Warn, Error };

/// Minimal process-wide logger. Info goes to stdout, Warn/Error to stderr.
/// A sink may additionally be installed to tee every emitted message
/// somewhere else (the web GUI uses one to stream a running job's progress
/// to the browser); sink installation and dispatch are mutex-guarded so a
/// background worker thread can log while the main thread reads.
class Logger {
public:
    /// Receives every message that passes the verbosity filter, in addition
    /// to the normal stdout/stderr output.
    using Sink = std::function<void(LogLevel, const std::string&)>;

    static void setVerbose(bool verbose);

    /// Forces colour on or off. Left alone, each stream is decided
    /// independently by whether it is a terminal, so redirecting only one of
    /// stdout/stderr does not put escape codes into the redirected one.
    static void setColorEnabled(bool enabled);

    /// Installs (or, with an empty function, removes) the message sink.
    static void setSink(Sink sink);

    static void debug(const std::string& message);
    static void info(const std::string& message);
    static void warn(const std::string& message);
    static void error(const std::string& message);

private:
    static void log(LogLevel level, const std::string& message);
};

} // namespace abp
