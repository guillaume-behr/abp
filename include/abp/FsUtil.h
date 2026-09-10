#pragma once

#include <filesystem>
#include <string>

namespace abp::fsutil {

namespace fs = std::filesystem;

/// Creates `dir` (and any missing parents) if it does not already exist.
/// Returns false only on an actual filesystem error.
bool ensureDirectory(const fs::path& dir);

/// Returns the size in bytes of the file at `path`, or 0 if it does not
/// exist or is not a regular file.
unsigned long long fileSize(const fs::path& path);

/// Reads the whole file at `path` into a string. Throws std::runtime_error
/// if it cannot be opened.
std::string readTextFile(const fs::path& path);

/// Writes `content` to `path`, overwriting any existing file. Throws
/// std::runtime_error if the file cannot be created.
void writeTextFile(const fs::path& path, const std::string& content);

/// Recursively sums the size of every regular file under `dir`.
unsigned long long directorySize(const fs::path& dir);

/// Sanitizes an Android package name for use as a filesystem path segment.
/// Package names are already restricted to [A-Za-z0-9_.], so this mainly
/// guards against unexpected input reaching the filesystem layer.
std::string sanitizeForFilename(const std::string& name);

} // namespace abp::fsutil
