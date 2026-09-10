#include "abp/ArchiveIntegrity.h"

#include <exception>

#include "abp/Logger.h"
#include "abp/Sha256.h"

namespace abp::integrity {

std::string checksumOrEmpty(const std::filesystem::path& path) {
    try {
        return crypto::sha256HexFile(path.string());
    } catch (const std::exception& e) {
        Logger::warn("Could not checksum " + path.string() + ": " + e.what());
        return std::string();
    }
}

bool checksumMatches(const std::filesystem::path& path, const std::string& expected) {
    if (expected.empty()) return true;
    try {
        return crypto::sha256HexFile(path.string()) == expected;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace abp::integrity
