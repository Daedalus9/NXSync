#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

constexpr unsigned LocalResolutionVersion = 2;

struct PendingLocalResolution {
    unsigned version{LocalResolutionVersion};
    std::uint64_t sequence{0};
    std::string storageEnvironment;
    std::string titleId;
    std::string profileUid;
    std::string parentRevisionId;
    std::string parentPayloadSha256;
    std::vector<std::string> parentRevisionIds;
};

bool validatePendingLocalResolution(
    const PendingLocalResolution& resolution,
    std::string& error);
std::string serializePendingLocalResolution(
    const PendingLocalResolution& resolution);
bool parsePendingLocalResolution(
    const std::string& text,
    PendingLocalResolution& resolution,
    std::string& error);
bool loadPendingLocalResolution(
    const std::string& path,
    PendingLocalResolution& resolution,
    std::string& error);
bool writePendingLocalResolutionAtomic(
    const std::string& path,
    const PendingLocalResolution& resolution,
    int& systemError);
bool completePendingLocalResolution(
    const std::string& path,
    std::uint64_t expectedSequence,
    int& systemError);

} // namespace nxsync
