#include "abp/FsUtil.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace abp::fsutil {

bool ensureDirectory(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        return true;
    }
    if (fs::exists(dir, ec)) {
        return false; // Exists but is a file/socket/... -- not usable as a directory.
    }

    ec.clear();
    fs::create_directories(dir, ec);
    if (!ec) return true;

    // A concurrent creator may have won the race; that is still success.
    ec.clear();
    return fs::is_directory(dir, ec);
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
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }

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

    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return 0;

    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        // Each query needs a fresh error_code: reusing a sticky one from an
        // earlier failure would make every later entry look like a failure.
        std::error_code entryEc;
        if (it->is_regular_file(entryEc) && !entryEc) {
            total += fileSize(it->path());
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
