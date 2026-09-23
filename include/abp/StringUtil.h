#pragma once

#include <string>
#include <vector>

namespace abp::strutil {

/// Splits `text` on every occurrence of `delimiter`. Empty fields are kept,
/// mirroring the behaviour most CLI users expect from e.g. `split(",")`.
std::vector<std::string> split(const std::string& text, char delimiter);

/// Joins `parts` with `separator` between each element.
std::string join(const std::vector<std::string>& parts, const std::string& separator);

/// Trims ASCII whitespace from both ends of `text`.
std::string trim(const std::string& text);

bool startsWith(const std::string& text, const std::string& prefix);
bool endsWith(const std::string& text, const std::string& suffix);

/// Wraps `arg` in single quotes for safe inclusion in a POSIX shell command
/// string, escaping any embedded single quotes. Use this for every value
/// (package name, path, ...) interpolated into a command that will be
/// executed by the on-device shell (e.g. via `adb shell` or `su -c`).
std::string shellQuote(const std::string& arg);

/// Returns true if `name` looks like a syntactically valid Android package
/// name (dot-separated identifiers). Used to defend against shell
/// injection before a package name is embedded into an on-device command.
bool isValidPackageName(const std::string& name);

/// Formats a byte count as a short human-readable string, e.g. "12.3 MB".
std::string formatBytes(unsigned long long bytes);

/// The current time as an ISO 8601 UTC timestamp, e.g. "2026-01-01T12:00:00Z".
std::string utcTimestamp();

} // namespace abp::strutil
