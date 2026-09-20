#pragma once

#include "nxsync/app_config.hpp"
#include "nxsync/nextcloud_paths.hpp"

#include <switch.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

struct NextcloudProgress {
    std::string stage;
    std::string remotePath;
    std::uint64_t bytesTransferred{0};
    std::uint64_t totalBytes{0};
};

using NextcloudProgressCallback = void (*)(
    const NextcloudProgress& progress,
    void* context);

struct NextcloudResult {
    bool success{false};
    Result networkResult{0};
    int curlCode{0};
    long httpStatus{0};
    std::string message;
    std::string remotePath;
    std::string remoteSha256;
    std::string serverResponse;
    std::uint64_t bytesUploaded{0};
    std::uint64_t bytesDownloaded{0};
};

std::string formatNextcloudFailure(const NextcloudResult& result);

struct NextcloudEntry {
    std::string name;
    std::string remotePath;
    bool directory{false};
    std::uint64_t size{0};
    std::string modified;
};

struct NextcloudListResult : NextcloudResult {
    std::vector<NextcloudEntry> entries;
};

struct NextcloudTextResult : NextcloudResult {
    std::string text;
};

class NextcloudClient {
public:
    explicit NextcloudClient(const AppConfig& config);
    ~NextcloudClient();

    NextcloudClient(const NextcloudClient&) = delete;
    NextcloudClient& operator=(const NextcloudClient&) = delete;

    bool ready() const;
    const NextcloudResult& initializationResult() const;

    NextcloudResult testConnection();
    NextcloudListResult listDirectory(const std::string& remotePath);
    NextcloudTextResult downloadText(
        const std::string& remotePath,
        std::size_t maximumBytes = 256 * 1024);
    NextcloudResult deleteFile(const std::string& remotePath);
    NextcloudResult downloadVerified(
        const std::string& remotePath,
        const std::string& localPath,
        NextcloudProgressCallback progressCallback = nullptr,
        void* progressContext = nullptr);
    NextcloudResult uploadVerified(
        const std::string& localPath,
        const std::string& remotePath,
        const std::string& sha256,
        NextcloudProgressCallback progressCallback = nullptr,
        void* progressContext = nullptr);
    NextcloudResult uploadTextVerified(
        const std::string& text,
        const std::string& remotePath,
        NextcloudProgressCallback progressCallback = nullptr,
        void* progressContext = nullptr);

private:
    AppConfig config_;
    std::string davBaseUrl_;
    bool socketsInitialized_{false};
    bool nifmInitialized_{false};
    bool curlInitialized_{false};
    NextcloudResult initializationResult_;
};

} // namespace nxsync
