#include "abp/BackupStore.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <unistd.h>

#include "TestFramework.h"
#include "abp/FsUtil.h"
#include "abp/Sha256.h"

using namespace abp;
namespace fs = std::filesystem;

namespace {

/// A throwaway directory tree that cleans itself up, so the store tests can
/// exercise real filesystem behaviour (symlinks included) rather than a mock.
class TempTree {
public:
    TempTree() {
        root_ = fs::temp_directory_path() /
                ("abp-store-test-" + std::to_string(reinterpret_cast<unsigned long long>(this)));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const fs::path& root() const { return root_; }

    void writeFile(const std::string& relative, const std::string& content) const {
        fs::path path = root_ / relative;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    /// Writes a minimal but valid manifest for a backup directory.
    void writeBackup(const std::string& relative, const std::string& createdAt, const std::string& mode) const {
        Manifest manifest;
        manifest.abpVersion = "1.0.0";
        manifest.createdAtUtc = createdAt;
        manifest.mode = mode;
        manifest.device.serial = "ABC123";
        manifest.device.model = "Pixel 8";
        manifest.device.manufacturer = "Google";

        PackageBackupEntry good;
        good.name = "com.example.good";
        good.dataIncluded = true;
        good.dataArchiveBytes = 2048;
        manifest.packages.push_back(good);

        PackageBackupEntry broken;
        broken.name = "com.example.broken";
        broken.error = "failed to capture app data via tar";
        manifest.packages.push_back(broken);

        fs::create_directories(root_ / relative);
        manifest.writeToFile(root_ / relative / "manifest.json");
    }

private:
    fs::path root_;
};

} // namespace

ABP_TEST(backupstore_detects_backup_directories) {
    TempTree tree;
    tree.writeBackup("good", "2026-01-01T00:00:00Z", "root");
    fs::create_directories(tree.root() / "empty");

    ABP_CHECK(BackupStore::isBackupDirectory(tree.root() / "good"));
    ABP_CHECK(!BackupStore::isBackupDirectory(tree.root() / "empty"));
    ABP_CHECK(!BackupStore::isBackupDirectory(tree.root() / "nope"));
}

ABP_TEST(backupstore_summarizes_a_backup) {
    TempTree tree;
    tree.writeBackup("good", "2026-01-01T00:00:00Z", "root");
    tree.writeFile("good/data/com.example.good.tar.gz", std::string(1024, 'x'));

    BackupSummaryInfo info = BackupStore::summarize(tree.root() / "good");
    ABP_CHECK_EQ(info.name, std::string("good"));
    ABP_CHECK_EQ(info.mode, std::string("root"));
    ABP_CHECK_EQ(info.createdAtUtc, std::string("2026-01-01T00:00:00Z"));
    ABP_CHECK_EQ(info.deviceModel, std::string("Pixel 8"));
    ABP_CHECK_EQ(info.packageCount, 2);
    ABP_CHECK_EQ(info.packagesWithData, 1);
    ABP_CHECK_EQ(info.packagesWithErrors, 1);
    ABP_CHECK(info.diskBytes >= 1024);
    ABP_CHECK_EQ(info.error, std::string(""));
}

ABP_TEST(backupstore_reports_a_corrupt_manifest_instead_of_throwing) {
    TempTree tree;
    tree.writeFile("broken/manifest.json", "{ this is not json");

    BackupSummaryInfo info = BackupStore::summarize(tree.root() / "broken");
    ABP_CHECK(!info.error.empty());
    ABP_CHECK_EQ(info.packageCount, 0);
}

ABP_TEST(backupstore_scan_finds_nested_backups_newest_first) {
    TempTree tree;
    tree.writeBackup("older", "2026-01-01T00:00:00Z", "standard");
    tree.writeBackup("nested/newer", "2026-06-01T00:00:00Z", "root");
    tree.writeBackup("too/deep/for/the/limit", "2026-09-01T00:00:00Z", "root");

    std::vector<BackupSummaryInfo> found = BackupStore::scan(tree.root(), 2);
    ABP_CHECK_EQ(found.size(), static_cast<size_t>(2));
    ABP_CHECK_EQ(found[0].name, std::string("newer"));
    ABP_CHECK_EQ(found[1].name, std::string("older"));

    // A backup directory is a leaf: scanning one directly returns just it.
    std::vector<BackupSummaryInfo> direct = BackupStore::scan(tree.root() / "older", 2);
    ABP_CHECK_EQ(direct.size(), static_cast<size_t>(1));
    ABP_CHECK_EQ(direct[0].name, std::string("older"));

    ABP_CHECK(BackupStore::scan(tree.root() / "does-not-exist", 2).empty());
}

ABP_TEST(backupstore_resolve_stays_inside_the_backup) {
    TempTree tree;
    tree.writeBackup("good", "2026-01-01T00:00:00Z", "root");
    tree.writeFile("good/apks/com.example.good/base.apk", "apk");
    tree.writeFile("outside-secret.txt", "secret");

    fs::path backup = tree.root() / "good";
    fs::path resolved;

    ABP_CHECK(BackupStore::resolveInside(backup, ".", &resolved));
    ABP_CHECK(BackupStore::resolveInside(backup, "apks/com.example.good", &resolved));
    ABP_CHECK(BackupStore::resolveInside(backup, "apks/com.example.good/base.apk", &resolved));
    ABP_CHECK_EQ(resolved.filename().string(), std::string("base.apk"));

    // Escapes of every shape are refused.
    ABP_CHECK(!BackupStore::resolveInside(backup, "..", &resolved));
    ABP_CHECK(!BackupStore::resolveInside(backup, "../outside-secret.txt", &resolved));
    ABP_CHECK(!BackupStore::resolveInside(backup, "apks/../../outside-secret.txt", &resolved));
    ABP_CHECK(!BackupStore::resolveInside(backup, "/etc/passwd", &resolved));
    // ...as are paths that simply are not there.
    ABP_CHECK(!BackupStore::resolveInside(backup, "no/such/file", &resolved));
}

ABP_TEST(backupstore_resolve_refuses_symlinks_pointing_out) {
    TempTree tree;
    tree.writeBackup("good", "2026-01-01T00:00:00Z", "root");
    tree.writeFile("outside-secret.txt", "secret");

    std::error_code ec;
    fs::create_symlink(tree.root() / "outside-secret.txt", tree.root() / "good" / "escape.txt", ec);
    if (ec) return; // Filesystem without symlink support; nothing to assert.

    fs::path resolved;
    ABP_CHECK(!BackupStore::resolveInside(tree.root() / "good", "escape.txt", &resolved));
}

ABP_TEST(backupstore_lists_directories_first) {
    TempTree tree;
    tree.writeBackup("good", "2026-01-01T00:00:00Z", "root");
    tree.writeFile("good/data/com.example.good.tar.gz", std::string(512, 'x'));
    tree.writeFile("good/apks/com.example.good/base.apk", "apk");

    std::vector<BackupFileEntry> entries = BackupStore::listDirectory(tree.root() / "good", ".");
    ABP_CHECK_EQ(entries.size(), static_cast<size_t>(3));
    ABP_CHECK_EQ(entries[0].name, std::string("apks"));
    ABP_CHECK(entries[0].isDirectory);
    ABP_CHECK_EQ(entries[1].name, std::string("data"));
    ABP_CHECK_EQ(entries[2].name, std::string("manifest.json"));
    ABP_CHECK(!entries[2].isDirectory);
    ABP_CHECK_EQ(entries[1].sizeBytes, static_cast<unsigned long long>(512));

    std::vector<BackupFileEntry> nested = BackupStore::listDirectory(tree.root() / "good", "apks");
    ABP_CHECK_EQ(nested.size(), static_cast<size_t>(1));
    ABP_CHECK_EQ(nested[0].relativePath, std::string("apks/com.example.good"));

    bool threw = false;
    try {
        BackupStore::listDirectory(tree.root() / "good", "../..");
    } catch (const std::exception&) {
        threw = true;
    }
    ABP_CHECK(threw);
}

ABP_TEST(backupstore_verify_reports_missing_and_tampered_files) {
    const fs::path dir = fs::temp_directory_path() / ("abp_verify_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fsutil::writeTextFile(dir / "data" / "good.tar.gz", "good");
    fsutil::writeTextFile(dir / "data" / "bad.tar.gz", "tampered");
    fsutil::writeTextFile(dir / "apks" / "a" / "base.apk", "apk");

    Manifest manifest;
    PackageBackupEntry good;
    good.name = "com.example.good";
    good.apkFiles = {"apks/a/base.apk"};
    good.dataArchive = "data/good.tar.gz";
    good.dataArchiveSha256 = crypto::sha256Hex("good");
    PackageBackupEntry bad;
    bad.name = "com.example.bad";
    bad.dataArchive = "data/bad.tar.gz";
    bad.dataArchiveSha256 = crypto::sha256Hex("original");
    bad.deDataArchive = "data/gone.de.tar.gz";
    bad.deDataArchiveSha256 = crypto::sha256Hex("x");
    PackageBackupEntry escaping;
    escaping.name = "com.example.escape";
    escaping.dataArchive = "../../etc/passwd";
    manifest.packages = {good, bad, escaping};
    manifest.writeToFile(dir / "manifest.json");

    VerifyReport report = BackupStore::verify(dir);
    fs::remove_all(dir);

    ABP_CHECK(!report.ok());
    ABP_CHECK_EQ(report.verified, 1);
    ABP_CHECK_EQ(report.unverifiable, 1); // The APK has no recorded checksum.
    ABP_CHECK_EQ(report.problems.size(), 3u);
    ABP_CHECK_EQ(report.problems[0].path, "data/bad.tar.gz");
    ABP_CHECK_EQ(report.problems[0].problem, "checksum mismatch");
    ABP_CHECK_EQ(report.problems[1].problem, "missing");
    ABP_CHECK_EQ(report.problems[2].path, "../../etc/passwd"); // Never followed outside the backup.
}
