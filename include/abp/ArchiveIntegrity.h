#pragma once

#include <filesystem>
#include <string>

namespace abp {

/// SHA-256 helpers shared by the backup backends.
///
/// crypto::sha256HexFile() throws when a file cannot be read. These wrappers
/// pick the right behaviour for each side of a backup: capturing a checksum
/// is best-effort, while verifying one must fail closed.
namespace integrity {

/// Checksums `path` for recording in the manifest, returning an empty string
/// (and warning) if it cannot be read. Losing a checksum is not worth
/// aborting a backup that has already been transferred.
std::string checksumOrEmpty(const std::filesystem::path& path);

/// True if `path` matches `expected`. An empty `expected` means the manifest
/// recorded no checksum, which passes. A file that cannot be read fails:
/// restoring from something unverifiable is exactly what the checksum is
/// there to prevent.
bool checksumMatches(const std::filesystem::path& path, const std::string& expected);

} // namespace integrity
} // namespace abp
