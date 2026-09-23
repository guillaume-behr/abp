#include "abp/BackupExplorer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <system_error>

#include "abp/BackupStore.h"
#include "abp/Process.h"
#include "abp/StringUtil.h"

namespace abp {
namespace fs = std::filesystem;
namespace {

std::string lowerExtension(const std::string& name) {
    std::string lower;
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const size_t dot = lower.rfind('.');
    return dot == std::string::npos ? std::string() : lower.substr(dot + 1);
}

/// Thumbnail caches and trash folders only duplicate (or never held) the
/// user's photos; ".thumbnails" alone can hold thousands of files.
bool isHiddenPath(const std::string& path) {
    for (const auto& segment : strutil::split(path, '/')) {
        if (!segment.empty() && segment[0] == '.') return true;
    }
    return false;
}

std::string parentOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

long long mtimeOf(const fs::path& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return 0;
    // file_time_type's epoch is unspecified before C++20; converting through
    // the difference from "now" on both clocks is portable.
    const auto now = std::chrono::system_clock::now() +
                     std::chrono::duration_cast<std::chrono::system_clock::duration>(time - fs::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

void scanDirectory(const fs::path& backupDir, const fs::path& root, std::vector<MediaItem>& out, size_t limit) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (const fs::recursive_directory_iterator end; it != end && out.size() < limit; it.increment(ec)) {
        if (ec) break;
        const std::string relToRoot = fs::relative(it->path(), root, ec).generic_string();
        if (isHiddenPath(relToRoot)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string kind = BackupExplorer::mediaKind(it->path().filename().string());
        if (kind.empty()) continue;
        MediaItem item;
        item.name = it->path().filename().string();
        item.folder = parentOf(relToRoot);
        item.sub = fs::relative(it->path(), backupDir, ec).generic_string();
        item.kind = kind;
        item.size = it->file_size(ec);
        item.mtime = mtimeOf(it->path());
        out.push_back(std::move(item));
    }
}

} // namespace

const tar::Entry* BackupExplorer::Archive::find(const std::string& member) const {
    for (const auto& entry : entries) {
        if (entry.name == member) return &entry;
    }
    return nullptr;
}

BackupExplorer::BackupExplorer(fs::path cacheDir) : cacheDir_(std::move(cacheDir)) {}

BackupExplorer::~BackupExplorer() {
    std::error_code ec;
    fs::remove_all(cacheDir_, ec);
}

std::shared_ptr<const BackupExplorer::Archive> BackupExplorer::openArchive(const fs::path& dir, const std::string& sub) {
    fs::path resolved;
    std::error_code ec;
    if (!BackupStore::resolveInside(dir, sub, &resolved) || !fs::is_regular_file(resolved, ec)) {
        throw std::runtime_error("No such archive in the backup: " + sub);
    }

    // Keyed by size and mtime too, so an archive rewritten by a new backup
    // into the same directory is re-read rather than served stale.
    const std::string key = resolved.string() + "|" + std::to_string(fs::file_size(resolved, ec)) + "|" +
                            std::to_string(mtimeOf(resolved));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = archives_.find(key);
        if (it != archives_.end()) return it->second;
    }

    auto archive = std::make_shared<Archive>();
    archive->tarFile = resolved;
    if (tar::isGzip(resolved)) {
        fs::path copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fs::create_directories(cacheDir_, ec);
            copy = cacheDir_ / ("archive_" + std::to_string(decompressed_++) + ".tar");
        }
        const ProcessResult r = Process::runToFile({"gzip", "-dc", resolved.string()}, copy.string());
        if (!r.ok()) {
            fs::remove(copy, ec);
            throw std::runtime_error(r.spawnFailed && !r.cancelled
                                         ? std::string("Opening .tar.gz archives needs 'gzip' on this computer.")
                                         : "Could not decompress " + sub + ": " + strutil::trim(r.stdErr));
        }
        archive->tarFile = copy;
    }
    archive->entries = tar::index(archive->tarFile);

    std::lock_guard<std::mutex> lock(mutex_);
    archives_[key] = archive;
    return archive;
}

std::vector<MediaItem> BackupExplorer::media(const fs::path& dir, size_t limit) {
    const Manifest manifest = BackupStore::loadManifest(dir);
    std::vector<MediaItem> items;

    if (manifest.sharedStorageIncluded) {
        if (manifest.sharedStorageIsDirectory) {
            fs::path root;
            if (BackupStore::resolveInside(dir, manifest.sharedStorageArchive, &root)) {
                scanDirectory(dir, root, items, limit);
            }
        } else {
            auto archive = openArchive(dir, manifest.sharedStorageArchive);
            for (const auto& entry : archive->entries) {
                if (items.size() >= limit) break;
                if (entry.type != tar::Entry::Type::File || isHiddenPath(entry.name)) continue;
                const std::string name = entry.name.substr(entry.name.rfind('/') + 1);
                const std::string kind = mediaKind(name);
                if (kind.empty()) continue;
                MediaItem item;
                item.name = name;
                item.folder = parentOf(entry.name);
                item.sub = manifest.sharedStorageArchive;
                item.member = entry.name;
                item.kind = kind;
                item.size = entry.size;
                item.mtime = entry.mtime;
                items.push_back(std::move(item));
            }
        }
    }

    // SD cards (and any /sdcard or /storage capture) land among the raw path captures.
    for (const auto& capture : manifest.filesystemCaptures) {
        if (!strutil::startsWith(capture.devicePath, "/storage/") && !strutil::startsWith(capture.devicePath, "/sdcard")) {
            continue;
        }
        fs::path root;
        if (BackupStore::resolveInside(dir, capture.localPath, &root)) scanDirectory(dir, root, items, limit);
    }

    std::stable_sort(items.begin(), items.end(), [](const MediaItem& a, const MediaItem& b) { return a.mtime > b.mtime; });
    return items;
}

bool BackupExplorer::isArchiveName(const std::string& name) {
    return strutil::endsWith(name, ".tar") || strutil::endsWith(name, ".tar.gz") || strutil::endsWith(name, ".tgz");
}

std::string BackupExplorer::mediaKind(const std::string& name) {
    const std::string ext = lowerExtension(name);
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "webp" || ext == "bmp" ||
        ext == "heic" || ext == "heif" || ext == "avif") {
        return "image";
    }
    if (ext == "mp4" || ext == "3gp" || ext == "webm" || ext == "mkv" || ext == "mov" || ext == "m4v") return "video";
    return std::string();
}

std::string BackupExplorer::contentTypeFor(const std::string& name) {
    static const std::map<std::string, std::string> kTypes = {
        {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"}, {"png", "image/png"},    {"gif", "image/gif"},
        {"webp", "image/webp"}, {"bmp", "image/bmp"},   {"heic", "image/heic"},  {"heif", "image/heif"},
        {"avif", "image/avif"}, {"mp4", "video/mp4"},   {"m4v", "video/mp4"},    {"3gp", "video/3gpp"},
        {"webm", "video/webm"}, {"mkv", "video/x-matroska"}, {"mov", "video/quicktime"},
        {"mp3", "audio/mpeg"},  {"m4a", "audio/mp4"},   {"aac", "audio/aac"},    {"ogg", "audio/ogg"},
        {"opus", "audio/ogg"},  {"wav", "audio/wav"},   {"amr", "audio/amr"},    {"flac", "audio/flac"},
        {"pdf", "application/pdf"},
    };
    // Shown as text, never interpreted: markup and scripts from the phone
    // included (html, svg, xml, js), since they would run on the GUI origin.
    static const char* kText[] = {"txt", "log",  "json", "xml", "html", "htm", "svg", "js",   "css", "csv",
                                  "vcf", "ics",  "conf", "ini", "md",   "prop", "properties", "sh", "yml", "yaml"};
    const std::string ext = lowerExtension(name);
    auto it = kTypes.find(ext);
    if (it != kTypes.end()) return it->second;
    for (const char* text : kText) {
        if (ext == text) return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

} // namespace abp
