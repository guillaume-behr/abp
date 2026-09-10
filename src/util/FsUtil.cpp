#include "abp/FsUtil.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace abp::fsutil {

bool ensureDirectory(const fs::path& dir) {
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        return fs::is_directory(dir, ec);
    }
    return fs::create_directories(dir, ec) || !ec;
}

unsigned long long fileSize(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return 0;
    }
    auto size = fs::file_size(path, ec);
    return ec ? 0ULL : static_cast<unsigned long long>(size);
}

std::string readTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open file for reading: " + path.string());
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

void writeTextFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("cannot open file for writing: " + path.string());
    }
    file << content;
    if (!file) {
        throw std::runtime_error("failed writing file: " + path.string());
    }
}

unsigned long long directorySize(const fs::path& dir) {
    std::error_code ec;
    unsigned long long total = 0;
    if (!fs::exists(dir, ec)) {
        return 0;
    }
    for (const auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec)) {
            total += fileSize(entry.path());
        }
    }
    return total;
}

std::string sanitizeForFilename(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (char rawChar : name) {
        unsigned char c = static_cast<unsigned char>(rawChar);
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('_');
        }
    }
    return result.empty() ? std::string("_") : result;
}

} // namespace abp::fsutil
