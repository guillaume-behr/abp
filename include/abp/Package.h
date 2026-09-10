#pragma once

#include <string>
#include <vector>

namespace abp {

/// A single installed package as enumerated from the device, plus what abp
/// found out about it while preparing a backup.
struct PackageInfo {
    std::string name;                    ///< e.g. "com.example.app"
    std::vector<std::string> apkPaths;    ///< On-device paths to base + split APKs.
    bool isSystemApp = false;
    bool hasData = true;                  ///< Whether app-private data should be captured.
};

} // namespace abp
