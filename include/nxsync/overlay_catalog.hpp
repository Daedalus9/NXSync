#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {

constexpr unsigned OverlayCatalogVersion = 1;

struct OverlayCatalogEntry {
    std::string profileUid;
    std::string profileName;
    std::string titleId;
    std::string titleName;
};

struct OverlayCatalog {
    unsigned version{OverlayCatalogVersion};
    std::uint64_t generatedUnix{0};
    std::vector<OverlayCatalogEntry> entries;
};

std::string serializeOverlayCatalog(const OverlayCatalog& catalog);
bool parseOverlayCatalog(
    const std::string& text,
    OverlayCatalog& catalog,
    std::string& error);
bool loadOverlayCatalog(
    const std::string& path,
    OverlayCatalog& catalog,
    std::string& error);
bool writeOverlayCatalogAtomic(
    const std::string& path,
    const OverlayCatalog& catalog,
    int& systemError);

bool findUniqueProfileUidForTitle(
    const OverlayCatalog& catalog,
    const std::string& titleId,
    std::string& profileUid);

} // namespace nxsync
