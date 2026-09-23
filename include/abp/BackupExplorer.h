#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "abp/TarArchive.h"

namespace abp {

/// A photo or video found anywhere in a backup, for the GUI's gallery.
struct MediaItem {
    std::string name;    ///< File name.
    std::string folder;  ///< Folder it sits in, relative to its storage root (e.g. "DCIM/Camera").
    std::string sub;     ///< File -- or containing archive -- relative to the backup directory.
    std::string member;  ///< Path inside that archive; empty for a plain file.
    std::string kind;    ///< "image" or "video".
    unsigned long long size = 0;
    long long mtime = 0; ///< Seconds since the epoch.
};

/// Read-side helpers behind the GUI's backup explorer: opening tar archives
/// inside a backup (so a root-mode shared_storage.tar or an app's
/// data/<pkg>.tar.gz can be browsed like a folder), and finding the photos
/// and videos a backup holds. Thread-safe; archive indexes are cached for
/// the lifetime of the object.
class BackupExplorer {
public:
    /// An archive ready to serve members from: `tarFile` is the uncompressed
    /// tar (the archive itself, or a decompressed copy in the cache).
    struct Archive {
        std::filesystem::path tarFile;
        std::vector<tar::Entry> entries;

        const tar::Entry* find(const std::string& member) const;
    };

    /// `cacheDir` holds decompressed copies of .tar.gz archives; it is
    /// created on demand and removed on destruction.
    explicit BackupExplorer(std::filesystem::path cacheDir);
    ~BackupExplorer();

    BackupExplorer(const BackupExplorer&) = delete;
    BackupExplorer& operator=(const BackupExplorer&) = delete;

    /// Opens the archive at `sub` inside backup `dir`. Throws
    /// std::runtime_error if the path escapes the backup or is not a
    /// readable tar (or gzipped tar) archive.
    std::shared_ptr<const Archive> openArchive(const std::filesystem::path& dir, const std::string& sub);

    /// Every photo and video in the backup's shared storage (directory or
    /// tar) and SD card captures, newest first, at most `limit` of them.
    std::vector<MediaItem> media(const std::filesystem::path& dir, size_t limit = 20000);

    /// True for names abp can open as an archive (.tar, .tar.gz, .tgz).
    static bool isArchiveName(const std::string& name);

    /// "image", "video", or "" for anything else, by extension.
    static std::string mediaKind(const std::string& name);

    /// The Content-Type to serve a backup file with. Anything a browser
    /// could execute (HTML, SVG, XML, ...) is served as plain text, since it
    /// comes from the phone and must never run on the GUI's origin.
    static std::string contentTypeFor(const std::string& name);

private:
    std::filesystem::path cacheDir_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const Archive>> archives_;
    int decompressed_ = 0;
};

} // namespace abp
