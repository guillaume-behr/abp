#pragma once

#include <functional>
#include <string>

namespace abp {

enum class LogLevel { Debug, Info, Warn, Error };

/// "debug", "info", "warn" or "error".
const char* logLevelName(LogLevel level);

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

    /// Receives progress through a long step: `done` of `total` items of
    /// `step` finished, `item` being the one now in hand.
    using ProgressSink = std::function<void(const std::string& step, int done, int total, const std::string& item)>;

    static void setVerbose(bool verbose);

    /// Forces colour on or off. Left alone, each stream is decided
    /// independently by whether it is a terminal, so redirecting only one of
    /// stdout/stderr does not put escape codes into the redirected one.
    static void setColorEnabled(bool enabled);

    /// Installs (or, with an empty function, removes) the message sink.
    static void setSink(Sink sink);

    /// Installs (or removes) the progress sink. With none installed, progress
    /// is drawn as a single self-overwriting line when stdout is a terminal,
    /// and not at all otherwise (a log file gains nothing from it).
    static void setProgressSink(ProgressSink sink);

    /// Reports progress through a step (see ProgressSink).
    static void progress(const std::string& step, int done, int total, const std::string& item = std::string());

    static void debug(const std::string& message);
    static void info(const std::string& message);
    static void warn(const std::string& message);
    static void error(const std::string& message);

private:
    static void log(LogLevel level, const std::string& message);
};

} // namespace abp
