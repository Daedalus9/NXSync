#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nxsync {

struct CloudWorkerStatus {
    unsigned version{1};
    std::string buildVersion;
    std::string state;
    std::string revisionId;
    std::string message;
    std::size_t completed{0};
    std::size_t failed{0};
    std::uint64_t bytesTransferred{0};
    std::uint64_t totalBytes{0};
};

constexpr unsigned CloudWorkerStatusVersion = 1;

std::string serializeCloudWorkerStatus(const CloudWorkerStatus& status);
bool parseCloudWorkerStatus(
    const std::string& text,
    CloudWorkerStatus& status,
    std::string& error);
bool loadCloudWorkerStatus(
    const std::string& path,
    CloudWorkerStatus& status,
    std::string& error);

} // namespace nxsync
