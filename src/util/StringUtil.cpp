#include "abp/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>

namespace abp::strutil {

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == delimiter) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

std::string join(const std::vector<std::string>& parts, const std::string& separator) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            result += separator;
        }
        result += parts[i];
    }
    return result;
}

std::string trim(const std::string& text) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && isSpace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && isSpace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string shellQuote(const std::string& arg) {
    std::string quoted = "'";
    for (char c : arg) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

bool isValidPackageName(const std::string& name) {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    static const std::regex pattern(R"(^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+$)");
    return std::regex_match(name, pattern);
}

std::string formatBytes(unsigned long long bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 4) {
        value /= 1024.0;
        ++unitIndex;
    }
    char buffer[64];
    if (unitIndex == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu %s", bytes, units[unitIndex]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unitIndex]);
    }
    return std::string(buffer);
}

} // namespace abp::strutil
