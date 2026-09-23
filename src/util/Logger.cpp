#include "abp/Logger.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unistd.h>
#include <utility>

namespace abp {
namespace {
std::atomic<bool> g_verbose{false};

/// Tri-state: unset means "decide per stream from isatty", which keeps escape
/// codes out of a redirected stdout even while stderr is still a terminal.
enum class ColorSetting { Auto, Always, Never };
ColorSetting g_colorSetting = ColorSetting::Auto;

bool colorEnabledFor(FILE* stream) {
    switch (g_colorSetting) {
        case ColorSetting::Always: return true;
        case ColorSetting::Never: return false;
        case ColorSetting::Auto: break;
    }
    return isatty(fileno(stream)) != 0;
}

std::mutex g_mutex;
Logger::Sink g_sink;
Logger::ProgressSink g_progressSink;

/// Whether a terminal progress line is currently drawn and must be erased
/// before anything else is printed. Guarded by g_mutex.
bool g_progressLineShown = false;

void clearProgressLineLocked() {
    if (!g_progressLineShown) return;
    std::fputs("\r\033[K", stdout);
    std::fflush(stdout);
    g_progressLineShown = false;
}

const char* levelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "\033[2m";     // dim
        case LogLevel::Info:  return "\033[0m";     // default
        case LogLevel::Warn:  return "\033[33m";    // yellow
        case LogLevel::Error: return "\033[31m";    // red
    }
    return "\033[0m";
}

} // namespace

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

void Logger::setVerbose(bool verbose) { g_verbose = verbose; }
void Logger::setColorEnabled(bool enabled) {
    g_colorSetting = enabled ? ColorSetting::Always : ColorSetting::Never;
}

void Logger::setSink(Sink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void Logger::setProgressSink(ProgressSink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_progressSink = std::move(sink);
}

void Logger::progress(const std::string& step, int done, int total, const std::string& item) {
    ProgressSink sink;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sink = g_progressSink;
    }
    if (sink) {
        sink(step, done, total, item);
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (isatty(fileno(stdout)) == 0 || total <= 0) return;
    std::string line = "  [" + std::to_string(done) + "/" + std::to_string(total) + "] " + step;
    if (!item.empty() && done < total) line += ": " + item;
    if (line.size() > 100) line = line.substr(0, 97) + "...";
    std::fprintf(stdout, "\r\033[K%s", line.c_str());
    std::fflush(stdout);
    g_progressLineShown = done < total;
    if (!g_progressLineShown) {
        std::fputs("\r\033[K", stdout);
        std::fflush(stdout);
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level == LogLevel::Debug && !g_verbose) {
        return;
    }

    // The sink is copied out and called with the lock released. Holding it
    // across the call would make the lock order "logger then whatever the
    // sink locks", and the GUI's sink takes the job runner's mutex -- one
    // logging call from inside that mutex would then deadlock. Copying also
    // keeps the target alive for the duration even if setSink() runs now.
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        sink = g_sink;
    }
    if (sink) sink(level, message);

    std::lock_guard<std::mutex> lock(g_mutex);
    clearProgressLineLocked();
    FILE* stream = (level == LogLevel::Warn || level == LogLevel::Error) ? stderr : stdout;
    const bool prefixed = level != LogLevel::Info;

    if (colorEnabledFor(stream)) {
        if (prefixed) {
            std::fprintf(stream, "%s[%s]\033[0m %s\n", levelColor(level), logLevelName(level), message.c_str());
        } else {
            std::fprintf(stream, "%s\n", message.c_str());
        }
    } else {
        if (prefixed) {
            std::fprintf(stream, "[%s] %s\n", logLevelName(level), message.c_str());
        } else {
            std::fprintf(stream, "%s\n", message.c_str());
        }
    }

    // stdout is block-buffered when it is not a terminal while stderr never
    // is, so without this every warning would surface ahead of the info lines
    // it belongs after whenever output is piped to a file or a log.
    std::fflush(stream);
}

void Logger::debug(const std::string& message) { log(LogLevel::Debug, message); }
void Logger::info(const std::string& message) { log(LogLevel::Info, message); }
void Logger::warn(const std::string& message) { log(LogLevel::Warn, message); }
void Logger::error(const std::string& message) { log(LogLevel::Error, message); }

} // namespace abp
