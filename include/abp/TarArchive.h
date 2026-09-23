#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace abp::tar {

/// One member of a tar archive, located by where its data starts in the
/// (uncompressed) archive file -- which is what lets the GUI serve a single
/// photo out of a multi-gigabyte shared_storage.tar without extracting it.
struct Entry {
    enum class Type { File, Directory, Symlink, Other };

    std::string name;             ///< Path inside the archive, without a leading "./" or "/".
    Type type = Type::File;
    unsigned long long size = 0;  ///< Data size in bytes (0 for directories and links).
    unsigned long long offset = 0;///< Byte offset of the data in the archive file.
    long long mtime = 0;          ///< Modification time, seconds since the epoch.
    std::string linkTarget;       ///< For symlinks.
};

/// Reads the headers of the uncompressed tar archive at `path` -- ustar,
/// GNU (long names via 'L' records) and pax (path/size overrides via 'x'
/// records), which covers what Android's toybox tar and host tars write.
/// Only headers are read; member data is skipped over. Throws
/// std::runtime_error if the file cannot be read or is not a tar archive.
std::vector<Entry> index(const std::filesystem::path& path);

/// True if `path` starts with the gzip magic bytes.
bool isGzip(const std::filesystem::path& path);

/// One level of an archive's directory tree, as a file browser shows it.
struct Child {
    std::string name;             ///< Last path segment.
    std::string path;             ///< Full path inside the archive.
    bool isDirectory = false;
    unsigned long long size = 0;  ///< For a directory, the total of the files under it.
    long long mtime = 0;
};

/// The immediate children of directory `prefix` ("" for the root, otherwise
/// ending in '/') among `entries`, directories first then by name.
/// Directories only implied by deeper paths are included.
std::vector<Child> listChildren(const std::vector<Entry>& entries, const std::string& prefix);

} // namespace abp::tar
