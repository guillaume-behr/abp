#include "abp/Manifest.h"

#include <stdexcept>

#include "abp/FsUtil.h"
#include "abp/Json.h"

namespace abp {
namespace {

using json::JsonValue;

JsonValue deviceToJson(const DeviceInfo& device) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("serial", device.serial);
    obj.set("model", device.model);
    obj.set("manufacturer", device.manufacturer);
    obj.set("android_release", device.androidRelease);
    obj.set("sdk_int", device.sdkInt);
    obj.set("rooted", device.isRooted());
    obj.set("root_method", device.root.method == RootMethod::AdbdRoot   ? "adbd_root"
                            : device.root.method == RootMethod::SuBinary ? "su_binary"
                                                                          : "none");
    return obj;
}

DeviceInfo deviceFromJson(const JsonValue& obj) {
    DeviceInfo device;
    device.serial = obj.get("serial").asString();
    device.model = obj.get("model").asString();
    device.manufacturer = obj.get("manufacturer").asString();
    device.androidRelease = obj.get("android_release").asString();
    device.sdkInt = static_cast<int>(obj.get("sdk_int").asInt());
    std::string method = obj.get("root_method").asString();
    if (method == "adbd_root") device.root.method = RootMethod::AdbdRoot;
    else if (method == "su_binary") device.root.method = RootMethod::SuBinary;
    else device.root.method = RootMethod::None;
    return device;
}

JsonValue packageToJson(const PackageBackupEntry& pkg) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("name", pkg.name);
    obj.set("system_app", pkg.isSystemApp);

    obj.set("apk_included", pkg.apkIncluded);
    JsonValue apkFiles = JsonValue::makeArray();
    for (const auto& f : pkg.apkFiles) apkFiles.push_back(JsonValue(f));
    obj.set("apk_files", apkFiles);

    obj.set("data_included", pkg.dataIncluded);
    obj.set("data_capture_method", dataCaptureMethodName(pkg.dataCaptureMethod));
    obj.set("data_archive", pkg.dataArchive);
    obj.set("data_archive_bytes", pkg.dataArchiveBytes);
    obj.set("data_archive_sha256", pkg.dataArchiveSha256);
    obj.set("de_data_archive", pkg.deDataArchive);
    obj.set("de_data_archive_bytes", pkg.deDataArchiveBytes);
    obj.set("de_data_archive_sha256", pkg.deDataArchiveSha256);

    obj.set("external_data_included", pkg.externalDataIncluded);
    obj.set("external_data_archive", pkg.externalDataArchive);
    obj.set("external_data_archive_bytes", pkg.externalDataArchiveBytes);
    obj.set("external_data_archive_sha256", pkg.externalDataArchiveSha256);

    obj.set("error", pkg.error);
    return obj;
}

PackageBackupEntry packageFromJson(const JsonValue& obj, const std::string& manifestMode) {
    PackageBackupEntry pkg;
    pkg.name = obj.get("name").asString();
    pkg.isSystemApp = obj.get("system_app").asBool();

    pkg.apkIncluded = obj.get("apk_included").asBool();
    JsonValue apkFiles = obj.get("apk_files");
    for (const auto& f : apkFiles.items()) pkg.apkFiles.push_back(f.asString());

    pkg.dataIncluded = obj.get("data_included").asBool();
    pkg.dataArchive = obj.get("data_archive").asString();
    pkg.dataArchiveBytes = static_cast<unsigned long long>(obj.get("data_archive_bytes").asInt());
    pkg.dataArchiveSha256 = obj.get("data_archive_sha256").asString();
    pkg.deDataArchive = obj.get("de_data_archive").asString();
    pkg.deDataArchiveBytes = static_cast<unsigned long long>(obj.get("de_data_archive_bytes").asInt());
    pkg.deDataArchiveSha256 = obj.get("de_data_archive_sha256").asString();

    pkg.externalDataIncluded = obj.get("external_data_included").asBool();
    pkg.externalDataArchive = obj.get("external_data_archive").asString();
    pkg.externalDataArchiveBytes =
        static_cast<unsigned long long>(obj.get("external_data_archive_bytes").asInt());
    pkg.externalDataArchiveSha256 = obj.get("external_data_archive_sha256").asString();

    pkg.error = obj.get("error").asString();

    if (obj.has("data_capture_method")) {
        pkg.dataCaptureMethod = dataCaptureMethodFromName(obj.get("data_capture_method").asString());
    } else if (!pkg.dataIncluded) {
        pkg.dataCaptureMethod = DataCaptureMethod::None;
    } else if (!pkg.dataArchive.empty()) {
        // Format version 1 only ever produced per-package archives in root
        // mode; standard mode had nothing but the legacy archive.
        pkg.dataCaptureMethod = DataCaptureMethod::RootTar;
    } else if (manifestMode == "standard") {
        pkg.dataCaptureMethod = DataCaptureMethod::LegacyAdbBackup;
    }
    return pkg;
}

JsonValue filesystemCaptureToJson(const FilesystemCapture& capture) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("device_path", capture.devicePath);
    obj.set("local_path", capture.localPath);
    obj.set("bytes", capture.bytes);
    obj.set("complete", capture.complete);
    obj.set("note", capture.note);
    return obj;
}

FilesystemCapture filesystemCaptureFromJson(const JsonValue& obj) {
    FilesystemCapture capture;
    capture.devicePath = obj.get("device_path").asString();
    capture.localPath = obj.get("local_path").asString();
    capture.bytes = static_cast<unsigned long long>(obj.get("bytes").asInt());
    capture.complete = obj.get("complete").asBool();
    capture.note = obj.get("note").asString();
    return capture;
}

JsonValue personalExportToJson(const PersonalDataExport& item) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("kind", item.kind);
    obj.set("format", item.format);
    obj.set("local_path", item.localPath);
    obj.set("item_count", item.itemCount);
    obj.set("bytes", item.bytes);
    obj.set("sha256", item.sha256);
    return obj;
}

PersonalDataExport personalExportFromJson(const JsonValue& obj) {
    PersonalDataExport item;
    item.kind = obj.get("kind").asString();
    item.format = obj.get("format").asString();
    item.localPath = obj.get("local_path").asString();
    item.itemCount = static_cast<int>(obj.get("item_count").asInt());
    item.bytes = static_cast<unsigned long long>(obj.get("bytes").asInt());
    item.sha256 = obj.get("sha256").asString();
    return item;
}

} // namespace

const char* dataCaptureMethodName(DataCaptureMethod method) {
    switch (method) {
        case DataCaptureMethod::RootTar: return "root_tar";
        case DataCaptureMethod::RunAsTar: return "run_as_tar";
        case DataCaptureMethod::LegacyAdbBackup: return "legacy_adb_backup";
        case DataCaptureMethod::None: return "none";
    }
    return "none";
}

DataCaptureMethod dataCaptureMethodFromName(const std::string& name) {
    if (name == "root_tar") return DataCaptureMethod::RootTar;
    if (name == "run_as_tar") return DataCaptureMethod::RunAsTar;
    if (name == "legacy_adb_backup") return DataCaptureMethod::LegacyAdbBackup;
    return DataCaptureMethod::None;
}

std::string Manifest::toJson() const {
    JsonValue root = JsonValue::makeObject();
    root.set("format_version", formatVersion);
    root.set("abp_version", abpVersion);
    root.set("created_at_utc", createdAtUtc);
    root.set("mode", mode);
    root.set("device", deviceToJson(device));

    root.set("shared_storage_included", sharedStorageIncluded);
    root.set("shared_storage_is_directory", sharedStorageIsDirectory);
    root.set("shared_storage_archive", sharedStorageArchive);
    root.set("shared_storage_archive_bytes", sharedStorageArchiveBytes);
    root.set("shared_storage_archive_sha256", sharedStorageArchiveSha256);

    root.set("legacy_adb_backup_file", legacyAdbBackupFile);

    JsonValue filesystemJson = JsonValue::makeArray();
    for (const auto& capture : filesystemCaptures) filesystemJson.push_back(filesystemCaptureToJson(capture));
    root.set("filesystem_captures", filesystemJson);

    JsonValue personalJson = JsonValue::makeArray();
    for (const auto& item : personalDataExports) personalJson.push_back(personalExportToJson(item));
    root.set("personal_data_exports", personalJson);

    JsonValue packagesJson = JsonValue::makeArray();
    for (const auto& pkg : packages) packagesJson.push_back(packageToJson(pkg));
    root.set("packages", packagesJson);

    return root.dump(2);
}

Manifest Manifest::fromJson(const std::string& text) {
    JsonValue root = JsonValue::parse(text);
    // Every field below is read with get(), which yields null on anything that
    // is not an object -- so "[]" or "42" would otherwise load as an empty,
    // perfectly valid-looking manifest instead of being reported as corrupt.
    if (!root.isObject()) {
        throw std::runtime_error("manifest.json is not a JSON object");
    }

    Manifest manifest;
    manifest.formatVersion = static_cast<int>(root.get("format_version").asInt(1));
    manifest.abpVersion = root.get("abp_version").asString();
    manifest.createdAtUtc = root.get("created_at_utc").asString();
    manifest.mode = root.get("mode").asString();
    manifest.device = deviceFromJson(root.get("device"));

    manifest.sharedStorageIncluded = root.get("shared_storage_included").asBool();
    manifest.sharedStorageIsDirectory = root.get("shared_storage_is_directory").asBool();
    manifest.sharedStorageArchive = root.get("shared_storage_archive").asString();
    manifest.sharedStorageArchiveBytes =
        static_cast<unsigned long long>(root.get("shared_storage_archive_bytes").asInt());
    manifest.sharedStorageArchiveSha256 = root.get("shared_storage_archive_sha256").asString();

    manifest.legacyAdbBackupFile = root.get("legacy_adb_backup_file").asString();

    // Bound to a named value: get() returns by value, so iterating
    // get(...).items() directly would walk a destroyed temporary.
    JsonValue capturesJson = root.get("filesystem_captures");
    for (const auto& captureJson : capturesJson.items()) {
        if (!captureJson.isObject()) continue;
        manifest.filesystemCaptures.push_back(filesystemCaptureFromJson(captureJson));
    }

    JsonValue personalJson = root.get("personal_data_exports");
    for (const auto& itemJson : personalJson.items()) {
        if (!itemJson.isObject()) continue;
        manifest.personalDataExports.push_back(personalExportFromJson(itemJson));
    }

    JsonValue packagesJson = root.get("packages");
    for (const auto& pkgJson : packagesJson.items()) {
        // A non-object entry carries no package at all; turning it into a
        // nameless entry would only surface later as a phantom package.
        if (!pkgJson.isObject()) continue;
        manifest.packages.push_back(packageFromJson(pkgJson, manifest.mode));
    }

    return manifest;
}

void Manifest::writeToFile(const std::filesystem::path& path) const {
    fsutil::writeTextFile(path, toJson());
}

Manifest Manifest::readFromFile(const std::filesystem::path& path) {
    return fromJson(fsutil::readTextFile(path));
}

PackageBackupEntry* findPackageEntry(Manifest& manifest, const std::string& packageName) {
    for (auto& entry : manifest.packages) {
        if (entry.name == packageName) return &entry;
    }
    return nullptr;
}

const PackageBackupEntry* findPackageEntry(const Manifest& manifest, const std::string& packageName) {
    for (const auto& entry : manifest.packages) {
        if (entry.name == packageName) return &entry;
    }
    return nullptr;
}

} // namespace abp
