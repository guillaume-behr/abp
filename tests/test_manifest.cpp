#include "abp/Manifest.h"
#include "TestFramework.h"

using namespace abp;

ABP_TEST(manifest_roundtrip) {
    Manifest manifest;
    manifest.formatVersion = 1;
    manifest.abpVersion = "1.0.0";
    manifest.createdAtUtc = "2026-01-01T00:00:00Z";
    manifest.mode = "root";

    manifest.device.serial = "ABC123";
    manifest.device.model = "Pixel 8";
    manifest.device.manufacturer = "Google";
    manifest.device.androidRelease = "15";
    manifest.device.sdkInt = 35;
    manifest.device.root.method = RootMethod::SuBinary;

    manifest.sharedStorageIncluded = true;
    manifest.sharedStorageIsDirectory = false;
    manifest.sharedStorageArchive = "shared_storage.tar";
    manifest.sharedStorageArchiveBytes = 12345;
    manifest.sharedStorageArchiveSha256 = "deadbeef";

    PackageBackupEntry pkg;
    pkg.name = "com.example.app";
    pkg.isSystemApp = false;
    pkg.apkIncluded = true;
    pkg.apkFiles = {"apks/com.example.app/base.apk"};
    pkg.dataIncluded = true;
    pkg.dataArchive = "data/com.example.app.tar.gz";
    pkg.dataArchiveBytes = 999;
    pkg.dataArchiveSha256 = "cafebabe";
    manifest.packages.push_back(pkg);

    PackageBackupEntry failed;
    failed.name = "com.example.broken";
    failed.error = "failed to capture app data via tar";
    manifest.packages.push_back(failed);

    Manifest parsed = Manifest::fromJson(manifest.toJson());

    ABP_CHECK_EQ(parsed.abpVersion, manifest.abpVersion);
    ABP_CHECK_EQ(parsed.mode, manifest.mode);
    ABP_CHECK_EQ(parsed.device.serial, manifest.device.serial);
    ABP_CHECK_EQ(parsed.device.sdkInt, manifest.device.sdkInt);
    ABP_CHECK(parsed.device.isRooted());
    ABP_CHECK(parsed.device.root.method == RootMethod::SuBinary);

    ABP_CHECK_EQ(parsed.sharedStorageArchiveBytes, manifest.sharedStorageArchiveBytes);
    ABP_CHECK_EQ(parsed.packages.size(), 2u);
    ABP_CHECK_EQ(parsed.packages[0].name, "com.example.app");
    ABP_CHECK_EQ(parsed.packages[0].dataArchiveSha256, "cafebabe");
    ABP_CHECK_EQ(parsed.packages[1].error, "failed to capture app data via tar");
}

ABP_TEST(manifest_find_package_entry) {
    Manifest manifest;
    PackageBackupEntry pkg;
    pkg.name = "com.example.app";
    manifest.packages.push_back(pkg);

    ABP_CHECK(findPackageEntry(manifest, "com.example.app") != nullptr);
    ABP_CHECK(findPackageEntry(manifest, "com.missing") == nullptr);

    const Manifest& constManifest = manifest;
    ABP_CHECK(findPackageEntry(constManifest, "com.example.app") != nullptr);
}

ABP_TEST(manifest_defaults_are_written_and_read_back) {
    Manifest empty;
    Manifest parsed = Manifest::fromJson(empty.toJson());

    ABP_CHECK_EQ(parsed.formatVersion, 3);
    ABP_CHECK_EQ(parsed.packages.size(), 0u);
    ABP_CHECK(!parsed.sharedStorageIncluded);
    ABP_CHECK(!parsed.device.isRooted());
}

ABP_TEST(manifest_preserves_the_format_version) {
    Manifest manifest;
    manifest.formatVersion = 7;
    ABP_CHECK_EQ(Manifest::fromJson(manifest.toJson()).formatVersion, 7);

    // A manifest with no format_version at all reads as version 1.
    ABP_CHECK_EQ(Manifest::fromJson("{}").formatVersion, 1);
}

ABP_TEST(manifest_preserves_large_archive_sizes) {
    // Sizes travel through JSON as doubles; a multi-gigabyte shared storage
    // capture must still round-trip exactly.
    Manifest manifest;
    manifest.sharedStorageArchiveBytes = 8589934592ULL; // 8 GiB
    ABP_CHECK_EQ(Manifest::fromJson(manifest.toJson()).sharedStorageArchiveBytes, 8589934592ULL);
}

ABP_TEST(manifest_rejects_malformed_json) {
    bool threw = false;
    try {
        Manifest::fromJson("{ this is not json");
    } catch (const std::exception&) {
        threw = true;
    }
    ABP_CHECK(threw);
}

ABP_TEST(manifest_round_trips_every_root_method) {
    for (RootMethod method : {RootMethod::None, RootMethod::SuBinary, RootMethod::AdbdRoot}) {
        Manifest manifest;
        manifest.device.root.method = method;
        Manifest parsed = Manifest::fromJson(manifest.toJson());
        ABP_CHECK(parsed.device.root.method == method);
    }
}

ABP_TEST(manifest_round_trips_every_data_capture_method) {
    for (DataCaptureMethod method : {DataCaptureMethod::None, DataCaptureMethod::RootTar,
                                      DataCaptureMethod::RunAsTar, DataCaptureMethod::LegacyAdbBackup}) {
        Manifest manifest;
        PackageBackupEntry pkg;
        pkg.name = "com.example.app";
        pkg.dataCaptureMethod = method;
        manifest.packages.push_back(pkg);

        Manifest parsed = Manifest::fromJson(manifest.toJson());
        ABP_CHECK(parsed.packages[0].dataCaptureMethod == method);
    }
}

ABP_TEST(manifest_v1_root_backup_infers_root_tar_capture) {
    // Format version 1 had no data_capture_method. A root-mode entry with a
    // per-package archive can only have come from a root tar.
    const std::string v1 = R"({
      "format_version": 1,
      "mode": "root",
      "packages": [
        {"name": "com.example.app", "data_included": true,
         "data_archive": "data/com.example.app.tar.gz"}
      ]
    })";

    Manifest parsed = Manifest::fromJson(v1);
    ABP_CHECK_EQ(parsed.formatVersion, 1);
    ABP_CHECK(parsed.packages[0].dataCaptureMethod == DataCaptureMethod::RootTar);
}

ABP_TEST(manifest_v1_standard_backup_infers_legacy_capture) {
    // In format version 1, standard mode had no per-package archives at all,
    // so any captured data came from the legacy adb backup archive.
    const std::string v1 = R"({
      "format_version": 1,
      "mode": "standard",
      "legacy_adb_backup_file": "legacy_backup.ab",
      "packages": [
        {"name": "com.example.app", "data_included": true, "data_archive": ""},
        {"name": "com.example.nodata", "data_included": false}
      ]
    })";

    Manifest parsed = Manifest::fromJson(v1);
    ABP_CHECK(parsed.packages[0].dataCaptureMethod == DataCaptureMethod::LegacyAdbBackup);
    ABP_CHECK(parsed.packages[1].dataCaptureMethod == DataCaptureMethod::None);
}

ABP_TEST(manifest_capture_method_names_round_trip) {
    ABP_CHECK_EQ(std::string(dataCaptureMethodName(DataCaptureMethod::RunAsTar)), "run_as_tar");
    ABP_CHECK(dataCaptureMethodFromName("run_as_tar") == DataCaptureMethod::RunAsTar);
    ABP_CHECK(dataCaptureMethodFromName("root_tar") == DataCaptureMethod::RootTar);
    ABP_CHECK(dataCaptureMethodFromName("legacy_adb_backup") == DataCaptureMethod::LegacyAdbBackup);
    // An unknown method from a future abp degrades to "nothing captured"
    // rather than being mistaken for a method this build understands.
    ABP_CHECK(dataCaptureMethodFromName("something_new") == DataCaptureMethod::None);
}

ABP_TEST(manifest_round_trips_filesystem_captures) {
    Manifest manifest;

    FilesystemCapture complete;
    complete.devicePath = "/system";
    complete.localPath = "filesystem/system";
    complete.bytes = 4096;
    complete.complete = true;
    manifest.filesystemCaptures.push_back(complete);

    FilesystemCapture partial;
    partial.devicePath = "/data";
    partial.localPath = "filesystem/data";
    partial.bytes = 8589934592ULL; // 8 GiB, past 32-bit range.
    partial.complete = false;
    partial.note = "Permission denied on /data/data";
    manifest.filesystemCaptures.push_back(partial);

    Manifest parsed = Manifest::fromJson(manifest.toJson());
    ABP_CHECK_EQ(parsed.filesystemCaptures.size(), 2u);
    ABP_CHECK_EQ(parsed.filesystemCaptures[0].devicePath, "/system");
    ABP_CHECK(parsed.filesystemCaptures[0].complete);
    ABP_CHECK_EQ(parsed.filesystemCaptures[1].devicePath, "/data");
    ABP_CHECK_EQ(parsed.filesystemCaptures[1].localPath, "filesystem/data");
    ABP_CHECK_EQ(parsed.filesystemCaptures[1].bytes, 8589934592ULL);
    ABP_CHECK(!parsed.filesystemCaptures[1].complete);
    ABP_CHECK_EQ(parsed.filesystemCaptures[1].note, "Permission denied on /data/data");
}

ABP_TEST(manifest_older_versions_have_no_filesystem_captures) {
    // A version 1 or 2 manifest predates the section entirely; reading one
    // must yield an empty list rather than tripping over the missing key.
    Manifest v2 = Manifest::fromJson(R"({"format_version": 2, "mode": "root", "packages": []})");
    ABP_CHECK_EQ(v2.formatVersion, 2);
    ABP_CHECK_EQ(v2.filesystemCaptures.size(), 0u);

    Manifest v1 = Manifest::fromJson(R"({"format_version": 1, "mode": "standard"})");
    ABP_CHECK_EQ(v1.filesystemCaptures.size(), 0u);
}

ABP_TEST(manifest_rejects_a_document_that_is_not_an_object) {
    for (const char* text : {"[]", "42", "\"manifest\"", "null"}) {
        bool threw = false;
        try {
            Manifest::fromJson(text);
        } catch (const std::exception&) {
            threw = true;
        }
        ABP_CHECK(threw);
    }
}

ABP_TEST(manifest_skips_package_entries_that_are_not_objects) {
    Manifest manifest = Manifest::fromJson(
        R"({"format_version": 3, "mode": "root", "packages": [42, {"name": "com.example.app"}, "junk"]})");
    ABP_CHECK_EQ(manifest.packages.size(), 1u);
    ABP_CHECK_EQ(manifest.packages[0].name, "com.example.app");
}
