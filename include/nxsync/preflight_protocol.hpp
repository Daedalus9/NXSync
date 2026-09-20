#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

constexpr unsigned PreflightProtocolVersion = 3;

struct PreflightRequest {
    unsigned version{PreflightProtocolVersion};
    std::uint64_t sequence{0};
    std::string titleId;
    std::string profileUid;
    bool automaticLaunch{false};
};

struct PreflightCandidate {
    std::string deviceId;
    std::string profileName;
    std::string revisionId;
    std::string payloadSha256;
    std::string archivePath;
    std::string gameVersion;
    std::string createdUtc;
};

struct PreflightStatus {
    unsigned version{PreflightProtocolVersion};
    std::string workerBuildVersion;
    std::uint64_t sequence{0};
    std::string titleId;
    std::string profileUid;
    std::string state;
    std::string outcome;
    std::string message;
    std::size_t validHeads{0};
    std::size_t invalidHeads{0};
    std::string selectedDeviceId;
    std::string selectedProfileName;
    std::string selectedRevisionId;
    std::string selectedArchivePath;
    std::string selectedGameVersion;
    std::string selectedCreatedUtc;
    std::string selectedPayloadSha256;
    std::string localRevisionId;
    std::string localPayloadSha256;
    std::vector<PreflightCandidate> candidates;
};

std::string serializePreflightRequest(const PreflightRequest& request);
bool parsePreflightRequest(
    const std::string& text,
    PreflightRequest& request,
    std::string& error);
bool loadPreflightRequest(
    const std::string& path,
    PreflightRequest& request,
    std::string& error);
bool writePreflightRequestAtomic(
    const std::string& path,
    const PreflightRequest& request,
    int& systemError);
bool completePreflightRequest(
    const std::string& path,
    std::uint64_t expectedSequence,
    int& systemError);

std::string serializePreflightStatus(const PreflightStatus& status);
bool parsePreflightStatus(
    const std::string& text,
    PreflightStatus& status,
    std::string& error);
bool loadPreflightStatus(
    const std::string& path,
    PreflightStatus& status,
    std::string& error);
bool writePreflightStatusAtomic(
    const std::string& path,
    const PreflightStatus& status,
    int& systemError);

} // namespace nxsync
