#include "abp/Logger.h"

#include <cstdio>
#include <mutex>
#include <unistd.h>
#include <utility>

namespace abp {
namespace {
bool g_verbose = false;
bool g_colorEnabled = isatty(fileno(stderr)) != 0;

std::mutex g_mutex;
Logger::Sink g_sink;

const char* levelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "\033[2m";     // dim
        case LogLevel::Info:  return "\033[0m";     // default
        case LogLevel::Warn:  return "\033[33m";    // yellow
        case LogLevel::Error: return "\033[31m";    // red
    }
    return "\033[0m";
}

const char* levelLabel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "?";
}
} // namespace

void Logger::setVerbose(bool verbose) { g_verbose = verbose; }
void Logger::setColorEnabled(bool enabled) { g_colorEnabled = enabled; }

void Logger::setSink(Sink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level == LogLevel::Debug && !g_verbose) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sink) g_sink(level, message);

    FILE* stream = (level == LogLevel::Warn || level == LogLevel::Error) ? stderr : stdout;
    const bool prefixed = level != LogLevel::Info;

    if (g_colorEnabled) {
        if (prefixed) {
            std::fprintf(stream, "%s[%s]\033[0m %s\n", levelColor(level), levelLabel(level), message.c_str());
        } else {
            std::fprintf(stream, "%s\n", message.c_str());
        }
    } else {
        if (prefixed) {
            std::fprintf(stream, "[%s] %s\n", levelLabel(level), message.c_str());
        } else {
            std::fprintf(stream, "%s\n", message.c_str());
        }
    }
}

void Logger::debug(const std::string& message) { log(LogLevel::Debug, message); }
void Logger::info(const std::string& message) { log(LogLevel::Info, message); }
void Logger::warn(const std::string& message) { log(LogLevel::Warn, message); }
void Logger::error(const std::string& message) { log(LogLevel::Error, message); }

} // namespace abp
