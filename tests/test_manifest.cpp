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
