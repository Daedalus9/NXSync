#pragma once

#include <switch.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

struct SaveEntry {
    std::uint64_t applicationId{0};
    std::uint64_t saveDataId{0};
    std::uint64_t rawSize{0};
    FsSaveDataSpaceId saveDataSpaceId{FsSaveDataSpaceId_User};
    std::uint16_t saveDataIndex{0};
    std::uint8_t saveDataRank{FsSaveDataRank_Primary};
    std::uint64_t commitId{0};
    std::uint64_t saveTimestamp{0};
    std::uint64_t ownerId{0};
    std::int64_t dataSize{0};
    std::int64_t journalSize{0};
    std::uint32_t flags{0};
    bool extraDataAvailable{false};
    std::string titleName;
    std::string gameVersion;
};

struct ApplicationSaveDataDefaults {
    bool available{false};
    Result result{0};
    std::uint64_t ownerId{0};
    std::uint64_t dataSize{0};
    std::uint64_t journalSize{0};
    std::uint64_t dataSizeMax{0};
    std::uint64_t journalSizeMax{0};
    std::string titleName;
    std::string gameVersion;
};

struct UserSaves {
    AccountUid uid{};
    std::string nickname;
    bool registeredProfile{false};
    std::vector<SaveEntry> saves;
};

struct SaveCatalog {
    std::vector<UserSaves> users;
    std::size_t totalSaves{0};
    std::size_t registeredProfiles{0};
    bool accountServiceAvailable{false};
    bool titleServiceAvailable{false};
    Result accountResult{0};
    Result saveReaderResult{0};
    Result titleServiceResult{0};

    bool ok() const { return R_SUCCEEDED(saveReaderResult); }
};

SaveCatalog loadSaveCatalog();
ApplicationSaveDataDefaults loadApplicationSaveDataDefaults(
    std::uint64_t applicationId);

std::string formatTitleId(std::uint64_t applicationId);
std::string formatByteSize(std::uint64_t bytes);
std::string formatResult(Result result);

} // namespace nxsync
