#include "nxsync/save_catalog.hpp"

#include "nxsync/display_text.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace nxsync {
namespace {

constexpr std::size_t ReaderBatchSize = 32;

bool uidEquals(const AccountUid& left, const AccountUid& right) {
    return left.uid[0] == right.uid[0] && left.uid[1] == right.uid[1];
}

std::string uidLabel(const AccountUid& uid) {
    char buffer[40]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "UID %08llX-%08llX",
        static_cast<unsigned long long>(uid.uid[1] & 0xFFFFFFFFULL),
        static_cast<unsigned long long>(uid.uid[0] & 0xFFFFFFFFULL));
    return buffer;
}

std::string cleanDisplayText(const char* value, const std::size_t maxLength) {
    return sanitizeDisplayUtf8(value, maxLength).value;
}

std::string bestLanguageName(NacpStruct& nacp, NacpLanguageEntry* preferred) {
    DisplayText selected;
    if (preferred != nullptr) {
        selected = sanitizeDisplayUtf8(preferred->name, sizeof(preferred->name));
    }
    if (selected.visibleCodepoints > 1) {
        return selected.value;
    }

    for (NacpLanguageEntry& candidate : nacp.lang) {
        const DisplayText cleaned = sanitizeDisplayUtf8(
            candidate.name,
            sizeof(candidate.name));
        if (cleaned.visibleCodepoints > selected.visibleCodepoints
            || (cleaned.visibleCodepoints == selected.visibleCodepoints
                && cleaned.value.size() > selected.value.size())) {
            selected = cleaned;
        }
    }
    return selected.value;
}

UserSaves* findUser(std::vector<UserSaves>& users, const AccountUid& uid) {
    const auto found = std::find_if(users.begin(), users.end(), [&](const UserSaves& user) {
        return uidEquals(user.uid, uid);
    });
    return found == users.end() ? nullptr : &*found;
}

void loadRegisteredProfiles(SaveCatalog& catalog) {
    // Save managers need the administrator account service (acc:su). The
    // application service (acc:u0) can return an empty/title-scoped user list.
    catalog.accountResult = accountInitialize(AccountServiceType_Administrator);
    if (R_FAILED(catalog.accountResult)) {
        return;
    }

    catalog.accountServiceAvailable = true;
    std::array<AccountUid, ACC_USER_LIST_SIZE> uids{};
    s32 actualTotal = 0;
    catalog.accountResult = accountListAllUsers(
        uids.data(),
        static_cast<s32>(uids.size()),
        &actualTotal);

    if (R_SUCCEEDED(catalog.accountResult)) {
        const s32 boundedTotal = std::max<s32>(
            0,
            std::min<s32>(actualTotal, static_cast<s32>(uids.size())));

        for (s32 index = 0; index < boundedTotal; ++index) {
            UserSaves user;
            user.uid = uids[static_cast<std::size_t>(index)];
            user.nickname = uidLabel(user.uid);
            user.registeredProfile = true;

            AccountProfile profile{};
            if (R_SUCCEEDED(accountGetProfile(&profile, user.uid))) {
                AccountProfileBase profileBase{};
                if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profileBase))) {
                    const std::string nickname = cleanDisplayText(
                        profileBase.nickname,
                        sizeof(profileBase.nickname));
                    if (!nickname.empty()) {
                        user.nickname = nickname;
                    }
                }
                accountProfileClose(&profile);
            }
            catalog.users.push_back(std::move(user));
        }
    }

    accountExit();
}

struct TitleMetadata {
    std::string name;
    std::string version;
};

TitleMetadata readTitleMetadata(const std::uint64_t applicationId) {
    // Reused for each title to avoid a 0x24000-byte stack allocation.
    static NsApplicationControlData controlData{};
    std::memset(&controlData, 0, sizeof(controlData));

    u64 actualSize = 0;
    const Result result = nsGetApplicationControlData(
        NsApplicationControlSource_Storage,
        applicationId,
        &controlData,
        sizeof(controlData),
        &actualSize);
    if (R_FAILED(result) || actualSize < sizeof(NacpStruct)) {
        return {};
    }

    TitleMetadata metadata;
    metadata.version = cleanDisplayText(
        controlData.nacp.display_version,
        sizeof(controlData.nacp.display_version));

    NacpLanguageEntry* languageEntry = nullptr;
    if (R_FAILED(nsGetApplicationDesiredLanguage(&controlData.nacp, &languageEntry))
        || languageEntry == nullptr) {
        nacpGetLanguageEntry(&controlData.nacp, &languageEntry);
    }
    metadata.name = bestLanguageName(controlData.nacp, languageEntry);
    return metadata;
}

void resolveTitleNames(SaveCatalog& catalog) {
    catalog.titleServiceResult = nsInitialize();
    if (R_FAILED(catalog.titleServiceResult)) {
        return;
    }
    catalog.titleServiceAvailable = true;

    std::map<std::uint64_t, TitleMetadata> cache;
    for (auto& user : catalog.users) {
        for (auto& save : user.saves) {
            const auto cached = cache.find(save.applicationId);
            if (cached != cache.end()) {
                save.titleName = cached->second.name;
                save.gameVersion = cached->second.version;
                continue;
            }

            TitleMetadata metadata = readTitleMetadata(save.applicationId);
            save.titleName = metadata.name;
            save.gameVersion = metadata.version;
            cache.emplace(save.applicationId, std::move(metadata));
        }
    }
    nsExit();
}

} // namespace

SaveCatalog loadSaveCatalog() {
    SaveCatalog catalog;
    loadRegisteredProfiles(catalog);

    FsSaveDataInfoReader reader{};
    catalog.saveReaderResult = fsOpenSaveDataInfoReader(
        &reader,
        FsSaveDataSpaceId_All);
    if (R_FAILED(catalog.saveReaderResult)) {
        return catalog;
    }

    std::array<FsSaveDataInfo, ReaderBatchSize> batch{};
    while (true) {
        s64 entriesRead = 0;
        catalog.saveReaderResult = fsSaveDataInfoReaderRead(
            &reader,
            batch.data(),
            batch.size(),
            &entriesRead);
        if (R_FAILED(catalog.saveReaderResult) || entriesRead <= 0) {
            break;
        }

        const auto boundedEntries = std::min<std::size_t>(
            static_cast<std::size_t>(entriesRead),
            batch.size());
        for (std::size_t index = 0; index < boundedEntries; ++index) {
            const FsSaveDataInfo& info = batch[index];
            if (info.save_data_type != FsSaveDataType_Account
                || info.save_data_rank != FsSaveDataRank_Primary
                || !accountUidIsValid(&info.uid)) {
                continue;
            }

            UserSaves* user = findUser(catalog.users, info.uid);
            if (user == nullptr) {
                // The save belongs to an AccountUid which is no longer a
                // registered local profile. It is deliberately excluded from
                // the catalog and therefore from future backup operations.
                continue;
            }

            SaveEntry entry;
            entry.applicationId = info.application_id;
            entry.saveDataId = info.save_data_id;
            entry.rawSize = info.size;
            entry.saveDataSpaceId = static_cast<FsSaveDataSpaceId>(info.save_data_space_id);
            entry.saveDataIndex = info.save_data_index;
            entry.saveDataRank = info.save_data_rank;
            FsSaveDataExtraData extraData{};
            if (R_SUCCEEDED(fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(
                    &extraData,
                    sizeof(extraData),
                    entry.saveDataSpaceId,
                    entry.saveDataId))) {
                entry.commitId = extraData.commit_id;
                entry.saveTimestamp = extraData.timestamp;
                entry.ownerId = extraData.owner_id;
                entry.dataSize = extraData.data_size;
                entry.journalSize = extraData.journal_size;
                entry.flags = extraData.flags;
                entry.extraDataAvailable = true;
            }
            user->saves.push_back(std::move(entry));
            ++catalog.totalSaves;
        }
    }
    fsSaveDataInfoReaderClose(&reader);

    if (R_FAILED(catalog.saveReaderResult)) {
        return catalog;
    }

    // A successful reader returns zero entries at EOF while keeping Result successful.
    for (auto& user : catalog.users) {
        std::sort(user.saves.begin(), user.saves.end(), [](const SaveEntry& left, const SaveEntry& right) {
            return left.applicationId < right.applicationId;
        });
    }
    std::sort(catalog.users.begin(), catalog.users.end(), [](const UserSaves& left, const UserSaves& right) {
        return left.nickname < right.nickname;
    });

    catalog.registeredProfiles = static_cast<std::size_t>(std::count_if(
        catalog.users.begin(),
        catalog.users.end(),
        [](const UserSaves& user) { return user.registeredProfile; }));

    resolveTitleNames(catalog);
    return catalog;
}

ApplicationSaveDataDefaults loadApplicationSaveDataDefaults(
    const std::uint64_t applicationId) {
    ApplicationSaveDataDefaults defaults;
    defaults.result = nsInitialize();
    if (R_FAILED(defaults.result)) {
        return defaults;
    }

    static NsApplicationControlData controlData{};
    std::memset(&controlData, 0, sizeof(controlData));
    u64 actualSize = 0;
    defaults.result = nsGetApplicationControlData(
        NsApplicationControlSource_Storage,
        applicationId,
        &controlData,
        sizeof(controlData),
        &actualSize);
    if (R_SUCCEEDED(defaults.result) && actualSize >= sizeof(NacpStruct)) {
        defaults.ownerId = controlData.nacp.save_data_owner_id;
        defaults.dataSize = controlData.nacp.user_account_save_data_size;
        defaults.journalSize = controlData.nacp.user_account_save_data_journal_size;
        defaults.dataSizeMax = controlData.nacp.user_account_save_data_size_max;
        defaults.journalSizeMax = controlData.nacp.user_account_save_data_journal_size_max;

        NacpLanguageEntry* languageEntry = nullptr;
        if (R_FAILED(nsGetApplicationDesiredLanguage(&controlData.nacp, &languageEntry))
            || languageEntry == nullptr) {
            nacpGetLanguageEntry(&controlData.nacp, &languageEntry);
        }
        if (languageEntry != nullptr) {
            defaults.titleName = bestLanguageName(controlData.nacp, languageEntry);
        }
        defaults.gameVersion = cleanDisplayText(
            controlData.nacp.display_version,
            sizeof(controlData.nacp.display_version));
        defaults.available = defaults.ownerId != 0
            && (defaults.dataSize != 0 || defaults.dataSizeMax != 0);
    }
    nsExit();
    return defaults;
}

} // namespace nxsync
