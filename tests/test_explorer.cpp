#include "abp/BackupExplorer.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "TestFramework.h"
#include "abp/FsUtil.h"
#include "abp/HttpServer.h"
#include "abp/Manifest.h"
#include "abp/Process.h"
#include "abp/TarArchive.h"

using namespace abp;
namespace fs = std::filesystem;

namespace {

/// A scratch directory holding a small tree, and tar archives of it made by
/// the host's own tar -- the reader is checked against real archives rather
/// than ones it wrote itself.
class TarFixture {
public:
    TarFixture() {
        static int counter = 0;
        dir_ = fs::temp_directory_path() / ("abp_tar_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        // Past ustar's 100-character name field, in segments ustar can still
        // split between its prefix and name fields.
        const std::string longDir = std::string(60, 'd') + "/" + std::string(60, 'e');
        fsutil::writeTextFile(dir_ / "tree" / "DCIM" / "Camera" / "IMG_0001.jpg", "JPEGDATA");
        fsutil::writeTextFile(dir_ / "tree" / "DCIM" / ".thumbnails" / "t.jpg", "THUMB");
        fsutil::writeTextFile(dir_ / "tree" / longDir / "deep.txt", "deep file");
        fsutil::writeTextFile(dir_ / "tree" / "Music" / "song.mp3", std::string(1500, 'm')); // Spans blocks.
    }
    ~TarFixture() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    TarFixture(const TarFixture&) = delete;
    TarFixture& operator=(const TarFixture&) = delete;

    /// Archives the tree's contents (as the device does: `tar -cf - -C /sdcard .`).
    fs::path make(const std::string& name, const std::string& format) const {
        const fs::path out = dir_ / name;
        ProcessResult r = Process::run({"tar", "--format=" + format, "-cf", out.string(), "-C", (dir_ / "tree").string(), "."});
        if (!r.ok()) throw std::runtime_error("host tar failed: " + r.stdErr);
        return out;
    }

    const fs::path& dir() const { return dir_; }

private:
    fs::path dir_;
};

std::string readSegment(const fs::path& file, unsigned long long offset, unsigned long long size) {
    std::ifstream in(file, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(size));
    return data;
}

const tar::Entry* findEntry(const std::vector<tar::Entry>& entries, const std::string& name) {
    for (const auto& entry : entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

} // namespace

ABP_TEST(tar_indexes_gnu_pax_and_ustar_archives) {
    TarFixture fixture;
    const std::string longName = std::string(60, 'd') + "/" + std::string(60, 'e') + "/deep.txt";
    for (const char* format : {"gnu", "pax", "ustar"}) {
        const fs::path archive = fixture.make(std::string("a_") + format + ".tar", format);
        const auto entries = tar::index(archive);

        const tar::Entry* photo = findEntry(entries, "DCIM/Camera/IMG_0001.jpg");
        ABP_CHECK(photo != nullptr);
        ABP_CHECK(photo->type == tar::Entry::Type::File);
        ABP_CHECK_EQ(photo->size, 8ULL);
        ABP_CHECK_EQ(readSegment(archive, photo->offset, photo->size), "JPEGDATA");
        ABP_CHECK(photo->mtime > 0);

        const tar::Entry* song = findEntry(entries, "Music/song.mp3");
        ABP_CHECK(song != nullptr);
        ABP_CHECK_EQ(readSegment(archive, song->offset, song->size), std::string(1500, 'm'));

        const tar::Entry* camera = findEntry(entries, "DCIM/Camera");
        ABP_CHECK(camera != nullptr);
        ABP_CHECK(camera->type == tar::Entry::Type::Directory);

        // GNU uses an 'L' record and pax an 'x' header for the long name;
        // plain ustar splits it into prefix + name.
        const tar::Entry* deep = findEntry(entries, longName);
        ABP_CHECK(deep != nullptr);
        ABP_CHECK_EQ(readSegment(archive, deep->offset, deep->size), "deep file");
    }
}

ABP_TEST(tar_refuses_what_is_not_a_tar_archive) {
    TarFixture fixture;
    fsutil::writeTextFile(fixture.dir() / "not.tar", std::string(2048, 'x'));
    bool threw = false;
    try {
        tar::index(fixture.dir() / "not.tar");
    } catch (const std::exception&) {
        threw = true;
    }
    ABP_CHECK(threw);
}

ABP_TEST(tar_lists_one_directory_level_with_implied_folders) {
    std::vector<tar::Entry> entries(3);
    entries[0].name = "DCIM/Camera/a.jpg";
    entries[0].size = 10;
    entries[1].name = "DCIM/Camera/b.jpg";
    entries[1].size = 5;
    entries[2].name = "notes.txt";
    entries[2].size = 1;

    auto root = tar::listChildren(entries, "");
    ABP_CHECK_EQ(root.size(), 2u);
    ABP_CHECK_EQ(root[0].name, "DCIM"); // Directories first, even if only implied.
    ABP_CHECK(root[0].isDirectory);
    ABP_CHECK_EQ(root[0].size, 15ULL);
    ABP_CHECK_EQ(root[1].name, "notes.txt");

    auto camera = tar::listChildren(entries, "DCIM/Camera/");
    ABP_CHECK_EQ(camera.size(), 2u);
    ABP_CHECK_EQ(camera[0].path, "DCIM/Camera/a.jpg");
}

ABP_TEST(explorer_opens_gzipped_archives_and_finds_media) {
    TarFixture fixture;
    const fs::path backup = fixture.dir() / "backup";
    fs::create_directories(backup / "data");
    fs::copy_file(fixture.make("shared.tar", "gnu"), backup / "shared_storage.tar");
    // An app data archive, gzipped like the ones abp writes.
    const fs::path appTar = fixture.make("app.tar", "gnu");
    ProcessResult gz = Process::runToFile({"gzip", "-c", appTar.string()}, (backup / "data" / "app.tar.gz").string());
    ABP_CHECK(gz.ok());

    Manifest manifest;
    manifest.mode = "root";
    manifest.sharedStorageIncluded = true;
    manifest.sharedStorageArchive = "shared_storage.tar";
    manifest.writeToFile(backup / "manifest.json");

    BackupExplorer explorer(fixture.dir() / "cache");
    auto app = explorer.openArchive(backup, "data/app.tar.gz");
    const tar::Entry* photo = app->find("DCIM/Camera/IMG_0001.jpg");
    ABP_CHECK(photo != nullptr);
    ABP_CHECK_EQ(readSegment(app->tarFile, photo->offset, photo->size), "JPEGDATA");
    ABP_CHECK(explorer.openArchive(backup, "data/app.tar.gz") == app); // Indexed once.

    auto media = explorer.media(backup);
    ABP_CHECK_EQ(media.size(), 1u); // .thumbnails and the mp3 are not gallery items.
    ABP_CHECK_EQ(media[0].name, "IMG_0001.jpg");
    ABP_CHECK_EQ(media[0].folder, "DCIM/Camera");
    ABP_CHECK_EQ(media[0].sub, "shared_storage.tar");
    ABP_CHECK_EQ(media[0].member, "DCIM/Camera/IMG_0001.jpg");

    bool escaped = false;
    try {
        explorer.openArchive(backup, "../app.tar");
    } catch (const std::exception&) {
        escaped = true;
    }
    ABP_CHECK(escaped);
}

ABP_TEST(explorer_never_serves_phone_content_as_something_runnable) {
    ABP_CHECK_EQ(BackupExplorer::contentTypeFor("IMG.JPG"), "image/jpeg");
    ABP_CHECK_EQ(BackupExplorer::contentTypeFor("clip.mp4"), "video/mp4");
    for (const char* risky : {"page.html", "icon.svg", "a.xml", "app.js", "x.HTM"}) {
        ABP_CHECK_EQ(BackupExplorer::contentTypeFor(risky), "text/plain; charset=utf-8");
    }
    ABP_CHECK_EQ(BackupExplorer::contentTypeFor("db.sqlite"), "application/octet-stream");
    ABP_CHECK_EQ(BackupExplorer::mediaKind("VID_1.MP4"), "video");
    ABP_CHECK_EQ(BackupExplorer::mediaKind("notes.txt"), "");
    ABP_CHECK(BackupExplorer::isArchiveName("data/app.tar.gz"));
    ABP_CHECK(!BackupExplorer::isArchiveName("legacy_backup.ab"));
}

ABP_TEST(http_parses_byte_ranges) {
    using http::RangeResult;
    unsigned long long start = 0;
    unsigned long long length = 0;
    ABP_CHECK(http::parseByteRange("", 100, &start, &length) == RangeResult::None);
    ABP_CHECK(http::parseByteRange("bytes=0-9", 100, &start, &length) == RangeResult::Satisfiable);
    ABP_CHECK_EQ(start, 0ULL);
    ABP_CHECK_EQ(length, 10ULL);
    ABP_CHECK(http::parseByteRange("bytes=90-", 100, &start, &length) == RangeResult::Satisfiable);
    ABP_CHECK_EQ(length, 10ULL);
    ABP_CHECK(http::parseByteRange("bytes=-30", 100, &start, &length) == RangeResult::Satisfiable);
    ABP_CHECK_EQ(start, 70ULL);
    ABP_CHECK(http::parseByteRange("bytes=50-500", 100, &start, &length) == RangeResult::Satisfiable);
    ABP_CHECK_EQ(length, 50ULL); // Clamped to the end.
    ABP_CHECK(http::parseByteRange("bytes=100-", 100, &start, &length) == RangeResult::Unsatisfiable);
    ABP_CHECK(http::parseByteRange("bytes=0-1,5-6", 100, &start, &length) == RangeResult::None);
    ABP_CHECK(http::parseByteRange("items=0-1", 100, &start, &length) == RangeResult::None);
    ABP_CHECK(http::parseByteRange("bytes=9-3", 100, &start, &length) == RangeResult::None);
}
