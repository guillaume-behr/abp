#include "abp/Logger.h"

#include <cstdio>
#include <unistd.h>

namespace abp {
namespace {
bool g_verbose = false;

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
void Logger::setColorEnabled(bool enabled) {
    g_colorSetting = enabled ? ColorSetting::Always : ColorSetting::Never;
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level == LogLevel::Debug && !g_verbose) {
        return;
    }

    FILE* stream = (level == LogLevel::Warn || level == LogLevel::Error) ? stderr : stdout;
    const bool prefixed = level != LogLevel::Info;

    if (colorEnabledFor(stream)) {
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
