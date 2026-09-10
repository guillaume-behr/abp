#pragma once

#include <string>
#include <vector>

namespace abp::devicepaths {

/// Why a requested device path cannot be pulled, or Ok if it can.
enum class PathVerdict {
    Ok,
    Empty,           ///< Not a path at all.
    NotAbsolute,     ///< Relative paths have no meaning on the device side.
    Root,            ///< Bare "/" -- would descend into the pseudo-filesystems.
    PseudoFilesystem,///< /proc, /sys, /dev and friends.
    Traversal,       ///< Contains a ".." segment.
};

/// Human-readable explanation for a non-Ok verdict, suitable for a CLI error.
std::string explainVerdict(PathVerdict verdict, const std::string& path);

/// Decides whether `path` is safe to hand to `adb pull`.
///
/// The dangerous cases are not hypothetical. `/proc/kcore` presents all of
/// physical memory as a single file; reading a character device under `/dev`
/// can block forever; `/sys` is an unbounded cyclic tree. A bare `/` reaches
/// all three, so it is refused in favour of naming real paths.
PathVerdict classify(const std::string& path);

/// Convenience: classify(path) == PathVerdict::Ok.
bool isPullable(const std::string& path);

/// Normalises `path` for use as a capture key: strips redundant and trailing
/// slashes, so "/data//app/" and "/data/app" name the same capture.
std::string normalize(const std::string& path);

/// True if `candidate` is `ancestor` or lives underneath it, comparing whole
/// path segments so "/datafoo" is not treated as being inside "/data".
bool isUnder(const std::string& candidate, const std::string& ancestor);

/// The roots `--all-files` pulls when the user names none: every persistent
/// partition an Android device normally has. Paths absent on a given device
/// are skipped at capture time rather than reported as failures.
std::vector<std::string> defaultCaptureRoots();

/// Turns `paths` into the set actually worth pulling: normalised, de-duped,
/// and with any path already covered by an earlier one dropped (pulling
/// /data and /data/app would otherwise copy /data/app twice).
std::vector<std::string> collapseRedundant(const std::vector<std::string>& paths);

} // namespace abp::devicepaths
