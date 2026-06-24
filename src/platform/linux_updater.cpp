#if defined(__linux__)

#include "platform/updater.hpp"

#include "config.hpp"
#include "ui/update_dialog.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <openssl/evp.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kVersionJsonPath =
    "/sonnam0904/sqleditor/releases/latest/download/version.json";
constexpr const char* kGitHubHost = "github.com";
constexpr const char* kUserAgent = "SQLEditor/" APP_VERSION;

struct ReleaseInfo {
    std::string version;
    std::string downloadUrl;
    std::string fileName;
    std::string sha256;
};

enum class CheckTrigger { Background, Manual };

struct UpdaterState {
    std::mutex mutex;
    std::future<std::optional<ReleaseInfo>> checkFuture;
    std::future<std::optional<std::string>> downloadFuture;
    CheckTrigger checkTrigger = CheckTrigger::Background;
    std::optional<ReleaseInfo> pendingRelease;
    std::string downloadedPath;
    std::string lastError;
    std::atomic<bool> updateAvailable{false};
    std::string latestVersion;
};

UpdaterState gState;

std::vector<int> parseVersionParts(std::string_view version) {
    std::vector<int> parts;
    std::string current;
    for (char c : version) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            current += c;
            continue;
        }
        if (!current.empty()) {
            parts.push_back(std::stoi(current));
            current.clear();
        }
        if (c == '-') {
            break;
        }
    }
    if (!current.empty()) {
        parts.push_back(std::stoi(current));
    }
    return parts;
}

bool isNewerVersion(const std::string& current, const std::string& latest) {
    const auto currentParts = parseVersionParts(current);
    const auto latestParts = parseVersionParts(latest);
    const std::size_t count = std::max(currentParts.size(), latestParts.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int a = i < currentParts.size() ? currentParts[i] : 0;
        const int b = i < latestParts.size() ? latestParts[i] : 0;
        if (b > a) {
            return true;
        }
        if (b < a) {
            return false;
        }
    }
    return false;
}

std::optional<ReleaseInfo> fetchLatestRelease() {
    httplib::Client cli(kGitHubHost);
    cli.set_connection_timeout(15);
    cli.set_read_timeout(60);
    cli.set_follow_location(true);
    cli.set_default_headers({{"User-Agent", kUserAgent}, {"Accept", "application/json"}});

    const auto res = cli.Get(kVersionJsonPath);
    if (!res || res->status != 200) {
        const std::string status = res ? std::to_string(res->status) : "no response";
        throw std::runtime_error("Failed to fetch release metadata (HTTP " + status + ")");
    }

    const json payload = json::parse(res->body);
    const auto& download = payload.at("downloads").at("linux-appimage-x86_64");

    ReleaseInfo info;
    info.version = payload.at("version").get<std::string>();
    info.downloadUrl = download.at("url").get<std::string>();
    info.fileName = download.at("file").get<std::string>();
    info.sha256 = download.at("sha256").get<std::string>();
    return info;
}

std::string sha256HexFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open downloaded file for verification");
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to initialize SHA-256 context");
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize SHA-256 digest");
    }

    std::array<char, 64 * 1024> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes = in.gcount();
        if (bytes > 0 && EVP_DigestUpdate(ctx, buffer.data(), static_cast<size_t>(bytes)) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("Failed to update SHA-256 digest");
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digestLen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("Failed to finalize SHA-256 digest");
    }
    EVP_MD_CTX_free(ctx);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digestLen * 2);
    for (unsigned int i = 0; i < digestLen; ++i) {
        hex.push_back(kHex[(digest[i] >> 4) & 0xF]);
        hex.push_back(kHex[digest[i] & 0xF]);
    }
    return hex;
}

fs::path resolveDownloadTarget(const ReleaseInfo& release) {
    if (const char* appImage = std::getenv("APPIMAGE")) {
        return fs::path(appImage).string() + ".new";
    }

    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".sqleditor" / "updates"
                         : fs::path("/tmp") / "sqleditor-updates";
    std::error_code ec;
    fs::create_directories(base, ec);
    return base / release.fileName;
}

std::optional<std::string> downloadRelease(const ReleaseInfo& release) {
    const auto target = resolveDownloadTarget(release);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);

    const std::size_t schemePos = release.downloadUrl.find("://");
    if (schemePos == std::string::npos) {
        throw std::runtime_error("Invalid download URL");
    }
    const std::size_t pathPos = release.downloadUrl.find('/', schemePos + 3);
    if (pathPos == std::string::npos) {
        throw std::runtime_error("Invalid download URL");
    }
    const std::string baseUrl = release.downloadUrl.substr(0, pathPos);
    const std::string path = release.downloadUrl.substr(pathPos);

    httplib::Client cli(baseUrl.c_str());
    cli.set_connection_timeout(15);
    cli.set_read_timeout(0);
    cli.set_follow_location(true);
    cli.set_default_headers({{"User-Agent", kUserAgent}});

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to create update file: " + target.string());
    }

    bool writeFailed = false;
    const auto res = cli.Get(path.c_str(), [&](const char* data, const std::size_t len) {
        out.write(data, static_cast<std::streamsize>(len));
        if (!out) {
            writeFailed = true;
            return false;
        }
        return true;
    });
    out.close();

    if (!res || res->status != 200 || writeFailed) {
        fs::remove(target, ec);
        const std::string status = res ? std::to_string(res->status) : "no response";
        throw std::runtime_error("Download failed (HTTP " + status + ")");
    }

    const std::string actualHash = sha256HexFile(target);
    if (actualHash != release.sha256) {
        fs::remove(target, ec);
        throw std::runtime_error("Downloaded file failed checksum verification");
    }

    fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    return target.string();
}

bool applyDownloadedUpdate(const std::string& downloadedPath) {
    if (const char* appImage = std::getenv("APPIMAGE")) {
        const fs::path target = appImage;
        const fs::path backup = target.string() + ".old";
        const fs::path staged = downloadedPath;

        std::error_code ec;
        fs::remove(backup, ec);
        ec.clear();
        fs::rename(target, backup, ec);
        if (ec) {
            spdlog::error("Failed to back up current AppImage: {}", ec.message());
            return false;
        }

        ec.clear();
        fs::rename(staged, target, ec);
        if (ec) {
            spdlog::error("Failed to install updated AppImage: {}", ec.message());
            fs::rename(backup, target, ec);
            return false;
        }

        execl(target.c_str(), target.c_str(), static_cast<char*>(nullptr));
        spdlog::error("Failed to restart updated AppImage");
        return false;
    }

    execl(downloadedPath.c_str(), downloadedPath.c_str(), static_cast<char*>(nullptr));
    spdlog::error("Failed to launch downloaded AppImage");
    return false;
}

void startVersionCheck(CheckTrigger trigger) {
    std::lock_guard lock(gState.mutex);
    if (gState.checkFuture.valid() &&
        gState.checkFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    gState.checkTrigger = trigger;
    if (trigger == CheckTrigger::Manual) {
        UpdateDialog::instance().showChecking();
    }

    gState.checkFuture = std::async(std::launch::async, []() -> std::optional<ReleaseInfo> {
        try {
            return fetchLatestRelease();
        } catch (const std::exception& e) {
            spdlog::warn("Update check failed: {}", e.what());
            return std::nullopt;
        }
    });
}

void handleCheckComplete() {
    std::optional<ReleaseInfo> release;
    CheckTrigger trigger = CheckTrigger::Background;
    {
        std::lock_guard lock(gState.mutex);
        if (!gState.checkFuture.valid() ||
            gState.checkFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        release = gState.checkFuture.get();
        trigger = gState.checkTrigger;
        gState.checkTrigger = CheckTrigger::Background;
    }

    if (!release) {
        if (trigger == CheckTrigger::Manual) {
            UpdateDialog::instance().showError(
                "Could not check for updates. Please verify your internet connection and try again.");
        }
        return;
    }

    const bool newer = isNewerVersion(APP_VERSION, release->version);
    const std::string latestVersion = release->version;
    {
        std::lock_guard lock(gState.mutex);
        gState.pendingRelease = newer ? std::move(release) : std::nullopt;
        gState.updateAvailable.store(newer);
        gState.latestVersion = newer ? latestVersion : "";
    }

    if (!newer) {
        if (trigger == CheckTrigger::Manual) {
            UpdateDialog::instance().showUpToDate();
        }
        return;
    }

    if (trigger == CheckTrigger::Manual) {
        UpdateDialog::instance().showUpdateAvailable(APP_VERSION, latestVersion, "");
    }
}

void startDownload() {
    std::optional<ReleaseInfo> release;
    {
        std::lock_guard lock(gState.mutex);
        if (!gState.pendingRelease || gState.downloadFuture.valid()) {
            return;
        }
        release = gState.pendingRelease;
    }

    UpdateDialog::instance().showDownloading();
    UpdateDialog::instance().clearWantsDownload();

    gState.downloadFuture = std::async(std::launch::async,
                                       [release = std::move(*release)]() -> std::optional<std::string> {
                                           try {
                                               return downloadRelease(release);
                                           } catch (const std::exception& e) {
                                               spdlog::error("Update download failed: {}", e.what());
                                               std::lock_guard lock(gState.mutex);
                                               gState.lastError = e.what();
                                               return std::nullopt;
                                           }
                                       });
}

void handleDownloadComplete() {
    std::optional<std::string> path;
    std::string error;
    {
        std::lock_guard lock(gState.mutex);
        if (!gState.downloadFuture.valid() ||
            gState.downloadFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        path = gState.downloadFuture.get();
        error = gState.lastError;
        gState.lastError.clear();
    }

    if (!path) {
        UpdateDialog::instance().showError(error.empty() ? "Failed to download update." : error);
        return;
    }

    {
        std::lock_guard lock(gState.mutex);
        gState.downloadedPath = *path;
    }
    UpdateDialog::instance().showDownloadComplete();
}

void handleRestartRequest() {
    std::string path;
    {
        std::lock_guard lock(gState.mutex);
        path = gState.downloadedPath;
    }
    if (path.empty()) {
        return;
    }

    UpdateDialog::instance().clearWantsRestart();
    if (!applyDownloadedUpdate(path)) {
        UpdateDialog::instance().showError(
            "Update downloaded but could not restart automatically. Please restart SQLEditor manually.");
    }
}

} // namespace

void initializeUpdater() {
    startVersionCheck(CheckTrigger::Background);
}

void checkForUpdates() {
    startVersionCheck(CheckTrigger::Manual);
}

void pollUpdater() {
    handleCheckComplete();
    handleDownloadComplete();

    if (UpdateDialog::instance().wantsDownload()) {
        startDownload();
    }
    if (UpdateDialog::instance().wantsRestart()) {
        handleRestartRequest();
    }

    UpdateDialog::instance().render();
}

void cleanupUpdater() {}

bool isUpdateAvailable() {
    return gState.updateAvailable.load();
}

std::string getLatestVersion() {
    std::lock_guard lock(gState.mutex);
    return gState.latestVersion;
}

#endif
