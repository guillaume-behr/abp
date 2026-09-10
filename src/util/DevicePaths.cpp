#include "abp/DevicePaths.h"

#include <algorithm>

#include "abp/StringUtil.h"

namespace abp::devicepaths {
namespace {

/// Kernel-backed pseudo-filesystems and device nodes. None of these hold
/// persistent data, and every one of them is hostile to a recursive copy.
const std::vector<std::string>& pseudoFilesystems() {
    static const std::vector<std::string> kPaths = {
        "/proc",           // /proc/kcore alone maps all of physical memory.
        "/sys",            // Unbounded, cyclic via symlinks.
        "/dev",            // Reading a character device can block forever.
        "/acct",           // cgroup accounting.
        "/config",         // configfs.
        "/debug_ramdisk",  //
        "/mnt/runtime",    // Bind-mount views of /sdcard; duplicates it.
        "/apex",           // Loopback-mounted APEX images, duplicated from /system.
        "/storage/self",   // Symlink loop back into /storage/emulated.
    };
    return kPaths;
}

} // namespace

std::string normalize(const std::string& path) {
    if (path.empty()) return path;

    std::string result;
    result.reserve(path.size());
    bool previousWasSlash = false;
    for (char c : path) {
        if (c == '/') {
            if (previousWasSlash) continue;
            previousWasSlash = true;
        } else {
            previousWasSlash = false;
        }
        result.push_back(c);
    }

    // Keep a lone "/" but drop any other trailing slash.
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

bool isUnder(const std::string& candidate, const std::string& ancestor) {
    const std::string a = normalize(candidate);
    const std::string b = normalize(ancestor);
    if (b == "/") return true;
    if (a == b) return true;
    // Compare on a segment boundary so "/datafoo" is not "under" "/data".
    return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/';
}

PathVerdict classify(const std::string& path) {
    const std::string normalized = normalize(strutil::trim(path));

    if (normalized.empty()) return PathVerdict::Empty;
    if (normalized[0] != '/') return PathVerdict::NotAbsolute;
    if (normalized == "/") return PathVerdict::Root;

    for (const auto& segment : strutil::split(normalized, '/')) {
        if (segment == "..") return PathVerdict::Traversal;
    }

    for (const auto& pseudo : pseudoFilesystems()) {
        if (isUnder(normalized, pseudo)) return PathVerdict::PseudoFilesystem;
    }
    return PathVerdict::Ok;
}

bool isPullable(const std::string& path) { return classify(path) == PathVerdict::Ok; }

std::string explainVerdict(PathVerdict verdict, const std::string& path) {
    switch (verdict) {
        case PathVerdict::Ok:
            return {};
        case PathVerdict::Empty:
            return "empty device path";
        case PathVerdict::NotAbsolute:
            return "'" + path + "' is not an absolute device path (it must start with '/')";
        case PathVerdict::Root:
            return "refusing to pull '/': it descends into /proc, /sys and /dev, which are kernel "
                   "pseudo-filesystems rather than stored files -- /proc/kcore alone exposes all of "
                   "physical memory, and reading a device node can block indefinitely. Use --all-files "
                   "for every persistent partition, or name real paths with --pull-path.";
        case PathVerdict::PseudoFilesystem:
            return "refusing to pull '" + path +
                   "': it is a kernel pseudo-filesystem or a bind-mount duplicate, not stored files";
        case PathVerdict::Traversal:
            return "'" + path + "' contains a '..' segment; name the target path directly";
    }
    return "unusable device path: '" + path + "'";
}

std::vector<std::string> defaultCaptureRoots() {
    // Every partition an Android device normally persists data on. Anything
    // missing on a given device is skipped at capture time.
    return {
        "/data",         // App and user data. Almost entirely root-only.
        "/sdcard",       // Shared storage / media.
        "/system",       // The OS image.
        "/system_ext",   //
        "/vendor",       //
        "/product",      //
        "/odm",          //
        "/oem",          //
        "/metadata",     //
    };
}

std::vector<std::string> collapseRedundant(const std::vector<std::string>& paths) {
    std::vector<std::string> normalized;
    normalized.reserve(paths.size());
    for (const auto& path : paths) {
        std::string clean = normalize(strutil::trim(path));
        if (clean.empty()) continue;
        if (std::find(normalized.begin(), normalized.end(), clean) == normalized.end()) {
            normalized.push_back(std::move(clean));
        }
    }

    // Shortest first, so an ancestor is always seen before its descendants.
    std::sort(normalized.begin(), normalized.end(),
              [](const std::string& a, const std::string& b) { return a.size() < b.size(); });

    std::vector<std::string> kept;
    for (const auto& path : normalized) {
        bool covered = false;
        for (const auto& existing : kept) {
            if (isUnder(path, existing)) {
                covered = true;
                break;
            }
        }
        if (!covered) kept.push_back(path);
    }

    // Report them in the order the caller gave, which is the order a user
    // reading the manifest would expect.
    std::vector<std::string> ordered;
    for (const auto& path : paths) {
        std::string clean = normalize(strutil::trim(path));
        if (std::find(kept.begin(), kept.end(), clean) != kept.end() &&
            std::find(ordered.begin(), ordered.end(), clean) == ordered.end()) {
            ordered.push_back(clean);
        }
    }
    return ordered;
}

} // namespace abp::devicepaths
