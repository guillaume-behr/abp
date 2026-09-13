#include "abp/FsUtil.h"

#include <unistd.h>

#include <fstream>
#include <string>

#include "TestFramework.h"

using namespace abp::fsutil;

namespace {

/// A scratch directory removed when it goes out of scope.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("abp_fsutil_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

} // namespace

ABP_TEST(fsutil_ensure_directory_creates_nested_paths) {
    TempDir tmp;
    fs::path nested = tmp.path() / "a" / "b" / "c";
    ABP_CHECK(ensureDirectory(nested));
    ABP_CHECK(fs::is_directory(nested));
    ABP_CHECK(ensureDirectory(nested)); // Idempotent.
}

ABP_TEST(fsutil_ensure_directory_rejects_a_path_occupied_by_a_file) {
    TempDir tmp;
    fs::path file = tmp.path() / "not_a_dir";
    writeFile(file, "x");
    ABP_CHECK(!ensureDirectory(file));
}

ABP_TEST(fsutil_file_size_reports_zero_for_missing_and_non_regular_paths) {
    TempDir tmp;
    ABP_CHECK_EQ(fileSize(tmp.path() / "missing"), 0ULL);
    ABP_CHECK_EQ(fileSize(tmp.path()), 0ULL); // A directory is not a regular file.

    fs::path file = tmp.path() / "sized";
    writeFile(file, std::string(1234, 'a'));
    ABP_CHECK_EQ(fileSize(file), 1234ULL);
}

ABP_TEST(fsutil_directory_size_sums_files_recursively) {
    TempDir tmp;
    ABP_CHECK_EQ(directorySize(tmp.path()), 0ULL);
    ABP_CHECK_EQ(directorySize(tmp.path() / "missing"), 0ULL);

    writeFile(tmp.path() / "one", std::string(100, 'x'));
    fs::create_directories(tmp.path() / "sub" / "deeper");
    writeFile(tmp.path() / "sub" / "two", std::string(20, 'x'));
    writeFile(tmp.path() / "sub" / "deeper" / "three", std::string(3, 'x'));

    ABP_CHECK_EQ(directorySize(tmp.path()), 123ULL);
}

ABP_TEST(fsutil_text_file_roundtrip) {
    TempDir tmp;
    fs::path file = tmp.path() / "nested" / "dir" / "doc.json";
    const std::string content = "{\"a\":1}\nline two\n";

    // Parent directories are created on demand.
    writeTextFile(file, content);
    ABP_CHECK_EQ(readTextFile(file), content);

    writeTextFile(file, "short"); // Truncates rather than appending.
    ABP_CHECK_EQ(readTextFile(file), "short");
}

ABP_TEST(fsutil_read_text_file_throws_for_a_missing_file) {
    TempDir tmp;
    bool threw = false;
    try {
        readTextFile(tmp.path() / "does_not_exist");
    } catch (const std::exception&) {
        threw = true;
    }
    ABP_CHECK(threw);
}

ABP_TEST(fsutil_sanitize_for_filename_replaces_path_separators) {
    ABP_CHECK_EQ(sanitizeForFilename("com.example.app"), "com.example.app");
    ABP_CHECK_EQ(sanitizeForFilename("com.example.app-2_x"), "com.example.app-2_x");
    ABP_CHECK_EQ(sanitizeForFilename("../../etc/passwd"), ".._.._etc_passwd");
    ABP_CHECK_EQ(sanitizeForFilename("a b;rm -rf /"), "a_b_rm_-rf__");
    ABP_CHECK_EQ(sanitizeForFilename(""), "_");
}
