#include "abp/GuiServer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <memory>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "abp/AdbClient.h"
#include "abp/BackupManager.h"
#include "abp/BackupOptions.h"
#include "abp/BackupStore.h"
#include "abp/HttpServer.h"
#include "abp/Json.h"
#include "abp/Logger.h"
#include "abp/Process.h"
#include "abp/StringUtil.h"
#include "abp/Version.h"
#include "abp/WebUi.h"

namespace abp {
namespace fs = std::filesystem;
namespace {

using json::JsonValue;

std::string utcTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

/// Expands a leading "~" to the user's home directory, so paths typed into
/// the GUI behave the way they do in a shell.
fs::path expandUserPath(const std::string& text) {
    if (text.empty()) return fs::path();
    if (text[0] == '~' && (text.size() == 1 || text[1] == '/')) {
        const char* home = std::getenv("HOME");
        if (home != nullptr) return fs::path(home) / text.substr(text.size() > 1 ? 2 : 1);
    }
    return fs::path(text);
}

std::string randomToken() {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    unsigned char bytes[16];
    if (urandom.read(reinterpret_cast<char*>(bytes), sizeof(bytes))) {
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned char byte : bytes) out << std::setw(2) << static_cast<int>(byte);
        return out.str();
    }

    std::random_device device;
    std::uniform_int_distribution<int> distribution(0, 15);
    const char* digits = "0123456789abcdef";
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) token.push_back(digits[distribution(device)]);
    return token;
}

/// Constant-time-ish comparison so a wrong token leaks nothing useful about
/// how much of it was right.
bool tokensMatch(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        difference = static_cast<unsigned char>(difference | (static_cast<unsigned char>(a[i]) ^
                                                              static_cast<unsigned char>(b[i])));
    }
    return difference == 0;
}

std::vector<std::string> stringArray(const JsonValue& value) {
    std::vector<std::string> result;
    if (!value.isArray()) return result;
    for (const auto& item : value.items()) {
        std::string text = strutil::trim(item.asString());
        if (!text.empty()) result.push_back(text);
    }
    return result;
}

bool boolField(const JsonValue& object, const std::string& key, bool def) {
    if (!object.has(key)) return def;
    return object.at(key).asBool(def);
}

http::Response jsonResponse(const JsonValue& value, int status = 200) {
    return http::Response::json(value.dump(2), status);
}

http::Response errorResponse(const std::string& message, int status) {
    JsonValue object = JsonValue::makeObject();
    object.set("error", message);
    return jsonResponse(object, status);
}

const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

const char* rootMethodLabel(RootMethod method) {
    switch (method) {
        case RootMethod::AdbdRoot: return "adbd already running as root";
        case RootMethod::SuBinary: return "su binary available";
        case RootMethod::None: return "none";
    }
    return "none";
}

JsonValue deviceJson(const DeviceInfo& device) {
    JsonValue object = JsonValue::makeObject();
    object.set("serial", device.serial);
    object.set("state", device.state);
    object.set("model", device.model);
    object.set("manufacturer", device.manufacturer);
    object.set("android_release", device.androidRelease);
    object.set("sdk_int", device.sdkInt);
    object.set("ready", device.isReady());
    object.set("rooted", device.isRooted());
    object.set("root_method", rootMethodLabel(device.root.method));
    return object;
}

JsonValue backupSummaryJson(const BackupSummaryInfo& info) {
    JsonValue object = JsonValue::makeObject();
    object.set("path", info.path.string());
    object.set("name", info.name);
    object.set("created_at", info.createdAtUtc);
    object.set("abp_version", info.abpVersion);
    object.set("mode", info.mode);
    object.set("device_serial", info.deviceSerial);
    object.set("device_model", info.deviceModel);
    object.set("device_manufacturer", info.deviceManufacturer);
    object.set("android_release", info.androidRelease);
    object.set("package_count", info.packageCount);
    object.set("packages_with_data", info.packagesWithData);
    object.set("packages_with_errors", info.packagesWithErrors);
    object.set("shared_storage_included", info.sharedStorageIncluded);
    object.set("disk_bytes", info.diskBytes);
    object.set("disk_size_human", strutil::formatBytes(info.diskBytes));
    object.set("error", info.error);
    return object;
}

JsonValue manifestJson(const Manifest& manifest) {
    JsonValue object = JsonValue::makeObject();
    object.set("format_version", manifest.formatVersion);
    object.set("abp_version", manifest.abpVersion);
    object.set("created_at", manifest.createdAtUtc);
    object.set("mode", manifest.mode);
    object.set("device", deviceJson(manifest.device));
    object.set("shared_storage_included", manifest.sharedStorageIncluded);
    object.set("shared_storage_is_directory", manifest.sharedStorageIsDirectory);
    object.set("shared_storage_archive", manifest.sharedStorageArchive);
    object.set("shared_storage_bytes", manifest.sharedStorageArchiveBytes);
    object.set("shared_storage_size_human", strutil::formatBytes(manifest.sharedStorageArchiveBytes));
    object.set("legacy_adb_backup_file", manifest.legacyAdbBackupFile);

    JsonValue packages = JsonValue::makeArray();
    for (const auto& entry : manifest.packages) {
        JsonValue item = JsonValue::makeObject();
        item.set("name", entry.name);
        item.set("system_app", entry.isSystemApp);
        item.set("apk_included", entry.apkIncluded);
        item.set("apk_count", static_cast<int>(entry.apkFiles.size()));
        JsonValue apkFiles = JsonValue::makeArray();
        for (const auto& file : entry.apkFiles) apkFiles.push_back(file);
        item.set("apk_files", apkFiles);
        item.set("data_included", entry.dataIncluded);
        item.set("data_archive", entry.dataArchive);
        item.set("data_bytes", entry.dataArchiveBytes);
        item.set("data_size_human", strutil::formatBytes(entry.dataArchiveBytes));
        item.set("data_sha256", entry.dataArchiveSha256);
        item.set("external_data_included", entry.externalDataIncluded);
        item.set("external_data_archive", entry.externalDataArchive);
        item.set("external_data_bytes", entry.externalDataArchiveBytes);
        item.set("external_data_size_human", strutil::formatBytes(entry.externalDataArchiveBytes));
        item.set("error", entry.error);
        packages.push_back(item);
    }
    object.set("packages", packages);
    return object;
}

/// The one backup or restore the GUI may have in flight, plus its captured
/// log. A single job at a time is a deliberate limitation: both operations
/// drive the same device over the same adb connection, and the CLI's
/// progress output is process-global.
class JobRunner {
public:
    struct Snapshot {
        bool present = false;
        int id = 0;
        std::string kind;
        std::string state;
        std::string target;
        std::string startedAt;
        std::string finishedAt;
        size_t logSize = 0;
        JsonValue result;
        std::vector<std::pair<std::string, std::string>> newLines;
    };

    bool busy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    /// Starts `work` on a background thread, teeing everything it logs into
    /// this job's log. Returns false (without starting anything) if a job is
    /// already running.
    bool start(const std::string& kind, const std::string& target, std::function<JsonValue()> work) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return false;

        if (thread_.joinable()) thread_.join();

        ++id_;
        kind_ = kind;
        target_ = target;
        state_ = "running";
        startedAt_ = utcTimestamp();
        finishedAt_.clear();
        log_.clear();
        result_ = JsonValue();
        running_ = true;
        present_ = true;

        thread_ = std::thread([this, work = std::move(work)]() {
            Logger::setSink([this](LogLevel level, const std::string& message) { append(levelName(level), message); });

            JsonValue result;
            std::string state = "succeeded";
            try {
                result = work();
                if (result.isObject() && !result.get("success").asBool(true)) state = "failed";
            } catch (const std::exception& e) {
                append("error", e.what());
                state = "failed";
            }

            Logger::setSink(nullptr);
            finish(state, std::move(result));
        });
        return true;
    }

    /// Current job state, plus every log line recorded at or after `since`.
    Snapshot snapshot(size_t since) const {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot snap;
        snap.present = present_;
        snap.id = id_;
        snap.kind = kind_;
        snap.state = state_;
        snap.target = target_;
        snap.startedAt = startedAt_;
        snap.finishedAt = finishedAt_;
        snap.logSize = log_.size();
        snap.result = result_;
        for (size_t i = std::min(since, log_.size()); i < log_.size(); ++i) snap.newLines.push_back(log_[i]);
        return snap;
    }

    /// Forgets a finished job so the GUI can return to its idle state.
    bool dismiss() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return false;
        present_ = false;
        log_.clear();
        result_ = JsonValue();
        return true;
    }

    void join() {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            thread = std::move(thread_);
        }
        if (thread.joinable()) thread.join();
    }

private:
    void append(const char* level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_.emplace_back(level, message);
    }

    void finish(const std::string& state, JsonValue result) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
        result_ = std::move(result);
        finishedAt_ = utcTimestamp();
        running_ = false;
    }

    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    bool present_ = false;
    int id_ = 0;
    std::string kind_;
    std::string state_ = "idle";
    std::string target_;
    std::string startedAt_;
    std::string finishedAt_;
    std::vector<std::pair<std::string, std::string>> log_;
    JsonValue result_;
};

JsonValue jobJson(const JobRunner::Snapshot& snap) {
    JsonValue object = JsonValue::makeObject();
    object.set("present", snap.present);
    if (!snap.present) return object;

    object.set("id", snap.id);
    object.set("kind", snap.kind);
    object.set("state", snap.state);
    object.set("target", snap.target);
    object.set("started_at", snap.startedAt);
    object.set("finished_at", snap.finishedAt);
    object.set("log_size", static_cast<long long>(snap.logSize));
    object.set("result", snap.result);

    JsonValue lines = JsonValue::makeArray();
    for (const auto& line : snap.newLines) {
        JsonValue item = JsonValue::makeObject();
        item.set("level", line.first);
        item.set("text", line.second);
        lines.push_back(item);
    }
    object.set("lines", lines);
    return object;
}

JsonValue backupSummaryResultJson(const BackupSummary& summary) {
    JsonValue object = JsonValue::makeObject();
    object.set("success", summary.success);
    object.set("mode", summary.mode);
    object.set("package_count", summary.packageCount);
    object.set("packages_with_data", summary.packagesWithData);
    object.set("packages_with_errors", summary.packagesWithErrors);
    object.set("shared_storage_included", summary.sharedStorageIncluded);
    object.set("total_bytes", summary.totalBytes);
    object.set("total_size_human", strutil::formatBytes(summary.totalBytes));
    object.set("output_dir", summary.outputDir.string());
    JsonValue messages = JsonValue::makeArray();
    for (const auto& message : summary.messages) messages.push_back(message);
    object.set("messages", messages);
    return object;
}

JsonValue restoreSummaryResultJson(const RestoreSummary& summary) {
    JsonValue object = JsonValue::makeObject();
    object.set("success", summary.success && summary.packagesFailed == 0);
    object.set("packages_restored", summary.packagesRestored);
    object.set("packages_failed", summary.packagesFailed);
    object.set("shared_storage_restored", summary.sharedStorageRestored);
    JsonValue messages = JsonValue::makeArray();
    for (const auto& message : summary.messages) messages.push_back(message);
    object.set("messages", messages);
    return object;
}

/// Routes requests to the API and serves the single-page app. Holds the
/// (thread-safe) job runner, so every request thread sees the same job.
class GuiApp {
public:
    GuiApp(GuiOptions options, std::shared_ptr<http::HttpServer> server)
        : options_(std::move(options)), server_(std::move(server)) {}

    http::Response handle(const http::Request& request) {
        if (!hostHeaderAllowed(request)) {
            return errorResponse("Request rejected: unexpected Host header.", 403);
        }

        if (request.path == "/" || request.path == "/index.html") {
            if (request.method != "GET") return errorResponse("Method not allowed", 405);
            return http::Response::html(webui::indexHtml());
        }

        if (!strutil::startsWith(request.path, "/api/")) return errorResponse("Not found", 404);

        if (!tokensMatch(options_.token, request.header("X-Abp-Token"))) {
            return errorResponse("Invalid or missing API token. Open the URL abp printed in your terminal.", 403);
        }

        try {
            return route(request);
        } catch (const json::JsonParseError& e) {
            return errorResponse(std::string("Malformed JSON in request body: ") + e.what(), 400);
        } catch (const std::exception& e) {
            return errorResponse(e.what(), 500);
        }
    }

    void joinJob() { job_.join(); }

private:
    /// Guards against DNS rebinding: a page on evil.example can resolve its
    /// own hostname to 127.0.0.1, but the Host header it sends will not be
    /// one of ours. (The API token is the primary defence; this is belt and
    /// braces, and only applies when bound to loopback.)
    bool hostHeaderAllowed(const http::Request& request) const {
        if (options_.host != "127.0.0.1" && options_.host != "localhost") return true;

        std::string host = request.header("Host");
        size_t colon = host.rfind(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
        return host.empty() || host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
    }

    http::Response route(const http::Request& request) {
        const std::string& path = request.path;
        const bool isPost = request.method == "POST";

        if (path == "/api/status") return handleStatus();
        if (path == "/api/devices") return handleDevices();
        if (path == "/api/device") return handleDevice(request);
        if (path == "/api/packages") return handlePackages(request);
        if (path == "/api/backups") return handleBackups(request);
        if (path == "/api/backup") return handleBackupDetail(request);
        if (path == "/api/backup/files") return handleBackupFiles(request);
        if (path == "/api/job") return handleJob(request);

        if (path == "/api/job/dismiss") {
            if (!isPost) return errorResponse("Method not allowed", 405);
            if (!job_.dismiss()) return errorResponse("A job is still running.", 409);
            return handleStatus();
        }
        if (path == "/api/jobs/backup") {
            if (!isPost) return errorResponse("Method not allowed", 405);
            return handleStartBackup(request);
        }
        if (path == "/api/jobs/restore") {
            if (!isPost) return errorResponse("Method not allowed", 405);
            return handleStartRestore(request);
        }
        if (path == "/api/shutdown") {
            if (!isPost) return errorResponse("Method not allowed", 405);
            if (job_.busy()) return errorResponse("A job is still running; wait for it to finish first.", 409);
            JsonValue object = JsonValue::makeObject();
            object.set("stopping", true);
            // Let the response reach the browser before the loop exits.
            std::thread([server = server_]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                server->stop();
            }).detach();
            return jsonResponse(object);
        }

        return errorResponse("Not found: " + path, 404);
    }

    http::Response handleStatus() {
        JsonValue object = JsonValue::makeObject();
        object.set("version", kVersionString);
        object.set("adb_path", AdbClient::adbPath());
        object.set("adb_available", AdbClient::isAdbAvailable());
        object.set("backup_root", options_.backupRoot.string());
        object.set("scan_depth", options_.scanDepth);
        object.set("job", jobJson(job_.snapshot(0)));
        return jsonResponse(object);
    }

    http::Response handleDevices() {
        JsonValue object = JsonValue::makeObject();
        object.set("adb_available", AdbClient::isAdbAvailable());
        JsonValue devices = JsonValue::makeArray();
        if (AdbClient::isAdbAvailable()) {
            for (const auto& device : AdbClient::listConnectedDevices()) devices.push_back(deviceJson(device));
        }
        object.set("devices", devices);
        return jsonResponse(object);
    }

    http::Response handleDevice(const http::Request& request) {
        if (!AdbClient::isAdbAvailable()) return errorResponse(adbMissingMessage(), 400);
        AdbClient adb(request.param("serial"));
        if (!adb.isConnected()) return errorResponse("No connected and authorized device found.", 404);
        return jsonResponse(deviceJson(adb.queryDeviceInfo()));
    }

    http::Response handlePackages(const http::Request& request) {
        if (!AdbClient::isAdbAvailable()) return errorResponse(adbMissingMessage(), 400);
        AdbClient adb(request.param("serial"));
        if (!adb.isConnected()) return errorResponse("No connected and authorized device found.", 404);

        bool includeSystem = request.param("system") == "1" || request.param("system") == "true";
        JsonValue packages = JsonValue::makeArray();
        for (const auto& package : adb.listPackages(includeSystem)) {
            JsonValue item = JsonValue::makeObject();
            item.set("name", package.name);
            item.set("system_app", package.isSystemApp);
            item.set("apk_count", static_cast<int>(package.apkPaths.size()));
            packages.push_back(item);
        }
        JsonValue object = JsonValue::makeObject();
        object.set("packages", packages);
        return jsonResponse(object);
    }

    http::Response handleBackups(const http::Request& request) {
        std::string rootText = request.param("root");
        fs::path root = rootText.empty() ? options_.backupRoot : expandUserPath(rootText);

        JsonValue object = JsonValue::makeObject();
        object.set("root", root.string());
        object.set("exists", fs::is_directory(root));

        JsonValue backups = JsonValue::makeArray();
        for (const auto& info : BackupStore::scan(root, options_.scanDepth)) {
            backups.push_back(backupSummaryJson(info));
        }
        object.set("backups", backups);
        return jsonResponse(object);
    }

    http::Response handleBackupDetail(const http::Request& request) {
        fs::path dir = expandUserPath(request.param("path"));
        if (dir.empty()) return errorResponse("Missing 'path' parameter.", 400);
        if (!BackupStore::isBackupDirectory(dir)) {
            return errorResponse("Not an abp backup directory (no manifest.json): " + dir.string(), 404);
        }

        JsonValue object = JsonValue::makeObject();
        object.set("summary", backupSummaryJson(BackupStore::summarize(dir)));
        try {
            object.set("manifest", manifestJson(BackupStore::loadManifest(dir)));
        } catch (const std::exception& e) {
            return errorResponse(std::string("Could not read manifest.json: ") + e.what(), 400);
        }
        return jsonResponse(object);
    }

    http::Response handleBackupFiles(const http::Request& request) {
        fs::path dir = expandUserPath(request.param("path"));
        if (dir.empty()) return errorResponse("Missing 'path' parameter.", 400);
        if (!BackupStore::isBackupDirectory(dir)) {
            return errorResponse("Not an abp backup directory (no manifest.json): " + dir.string(), 404);
        }

        std::string relative = request.param("sub", ".");
        if (relative.empty()) relative = ".";
        if (!BackupStore::resolveInside(dir, relative, nullptr)) {
            return errorResponse("Path is not inside the backup directory: " + relative, 403);
        }

        JsonValue entries = JsonValue::makeArray();
        for (const auto& entry : BackupStore::listDirectory(dir, relative)) {
            JsonValue item = JsonValue::makeObject();
            item.set("name", entry.name);
            item.set("path", entry.relativePath);
            item.set("directory", entry.isDirectory);
            item.set("size_bytes", entry.sizeBytes);
            item.set("size_human", strutil::formatBytes(entry.sizeBytes));
            entries.push_back(item);
        }

        JsonValue object = JsonValue::makeObject();
        object.set("path", dir.string());
        object.set("sub", relative == "." ? "" : relative);
        object.set("entries", entries);
        return jsonResponse(object);
    }

    http::Response handleJob(const http::Request& request) {
        size_t since = 0;
        std::string sinceText = request.param("since");
        if (!sinceText.empty()) {
            try {
                long long parsed = std::stoll(sinceText);
                if (parsed > 0) since = static_cast<size_t>(parsed);
            } catch (const std::exception&) {
                return errorResponse("'since' must be a number.", 400);
            }
        }
        JsonValue object = JsonValue::makeObject();
        object.set("job", jobJson(job_.snapshot(since)));
        return jsonResponse(object);
    }

    http::Response handleStartBackup(const http::Request& request) {
        JsonValue body = json::JsonValue::parse(request.body.empty() ? "{}" : request.body);

        BackupOptions options;
        options.serial = strutil::trim(body.get("serial").asString());
        options.outputDir = expandUserPath(strutil::trim(body.get("output").asString()));
        options.includeApks = boolField(body, "include_apks", true);
        options.includeAppData = boolField(body, "include_data", true);
        options.includeSharedStorage = boolField(body, "include_shared", true);
        options.includeSystemApps = boolField(body, "include_system", false);
        options.onlyPackages = stringArray(body.get("only"));
        options.excludePackages = stringArray(body.get("exclude"));
        options.assumeYes = true; // The browser already asked for confirmation.

        std::string mode = body.get("mode").asString("auto");
        if (mode == "root") options.mode = BackupMode::Root;
        else if (mode == "standard") options.mode = BackupMode::Standard;
        else if (mode == "auto" || mode.empty()) options.mode = BackupMode::Auto;
        else return errorResponse("Unknown backup mode: " + mode, 400);

        if (options.outputDir.empty()) return errorResponse("An output directory is required.", 400);
        if (!AdbClient::isAdbAvailable()) return errorResponse(adbMissingMessage(), 400);

        std::string target = options.outputDir.string();
        if (!job_.start("backup", target, [options]() { return backupSummaryResultJson(BackupManager::runBackup(options)); })) {
            return errorResponse("Another job is already running.", 409);
        }
        return acceptedJobResponse();
    }

    http::Response handleStartRestore(const http::Request& request) {
        JsonValue body = json::JsonValue::parse(request.body.empty() ? "{}" : request.body);

        RestoreOptions options;
        options.serial = strutil::trim(body.get("serial").asString());
        options.inputDir = expandUserPath(strutil::trim(body.get("input").asString()));
        options.includeApks = boolField(body, "include_apks", true);
        options.includeAppData = boolField(body, "include_data", true);
        options.includeSharedStorage = boolField(body, "include_shared", true);
        options.onlyPackages = stringArray(body.get("only"));
        options.excludePackages = stringArray(body.get("exclude"));
        options.assumeYes = true;

        if (options.inputDir.empty()) return errorResponse("A backup directory is required.", 400);
        if (!BackupStore::isBackupDirectory(options.inputDir)) {
            return errorResponse("Not an abp backup directory (no manifest.json): " + options.inputDir.string(), 400);
        }
        if (!AdbClient::isAdbAvailable()) return errorResponse(adbMissingMessage(), 400);

        std::string target = options.inputDir.string();
        if (!job_.start("restore", target,
                        [options]() { return restoreSummaryResultJson(BackupManager::runRestore(options)); })) {
            return errorResponse("Another job is already running.", 409);
        }
        return acceptedJobResponse();
    }

    http::Response acceptedJobResponse() {
        JsonValue object = JsonValue::makeObject();
        object.set("job", jobJson(job_.snapshot(0)));
        return jsonResponse(object, 202);
    }

    static std::string adbMissingMessage() {
        return "Could not run '" + AdbClient::adbPath() +
               "'. Install Android platform-tools and make sure adb is on your PATH.";
    }

    GuiOptions options_;
    std::shared_ptr<http::HttpServer> server_;
    JobRunner job_;
};

void openInBrowser(const std::string& url) {
    // Fire and forget: xdg-open hands off to the desktop's handler, and abp
    // should not care whether one exists.
    std::thread([url]() {
        ProcessResult result = Process::run({"xdg-open", url});
        if (!result.ok()) {
            Logger::debug("Could not open a browser automatically (xdg-open failed).");
        }
    }).detach();
}

} // namespace

int GuiServer::run(const GuiOptions& optionsIn) {
    GuiOptions options = optionsIn;
    if (options.token.empty()) options.token = randomToken();
    if (options.backupRoot.empty()) options.backupRoot = fs::current_path();
    options.backupRoot = expandUserPath(options.backupRoot.string());

    auto server = std::make_shared<http::HttpServer>();
    std::string error;
    if (!server->listen(options.host, options.port, &error)) {
        Logger::error(error);
        if (error.find("Could not bind") != std::string::npos && options.port != 0) {
            Logger::error("Is another abp gui already running? Try a different --port, or --port 0 to pick a free one.");
        }
        return 1;
    }

    std::string url = "http://" + (options.host == "0.0.0.0" ? std::string("127.0.0.1") : options.host) + ":" +
                      std::to_string(server->boundPort()) + "/?token=" + options.token;

    // Shared, not stack-owned: a request thread may still be finishing when
    // serveForever() returns, and it holds a copy of the handler.
    auto app = std::make_shared<GuiApp>(options, server);

    std::cout << "abp " << kVersionString << " web GUI\n";
    std::cout << "  Serving:      " << url << "\n";
    std::cout << "  Backup root:  " << options.backupRoot.string() << "\n";
    if (options.host != "127.0.0.1" && options.host != "localhost") {
        Logger::warn("Listening on " + options.host +
                     ": anyone who can reach this port and knows the token can back up and restore "
                     "the connected device.");
    }
    std::cout << "  Press Ctrl-C to stop.\n\n" << std::flush;

    if (options.openBrowser) openInBrowser(url);

    server->serveForever([app](const http::Request& request) { return app->handle(request); });

    app->joinJob();
    std::cout << "GUI stopped.\n";
    return 0;
}

} // namespace abp
