#include "nxsync/atomic_file.hpp"
#include "nxsync/overlay_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace nxsync {
namespace {

constexpr std::size_t MaximumEntries = 4096;

bool isHex(const std::string& value, const std::size_t length) {
    return value.size() == length
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

std::string escapeValue(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\') result += "\\\\";
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else if (ch == '=') result += "\\e";
        else result.push_back(ch);
    }
    return result;
}

bool unescapeValue(const std::string& value, std::string& output) {
    output.clear();
    bool escaped = false;
    for (const char ch : value) {
        if (!escaped && ch == '\\') escaped = true;
        else if (escaped) {
            if (ch == '\\') output.push_back('\\');
            else if (ch == 'n') output.push_back('\n');
            else if (ch == 'r') output.push_back('\r');
            else if (ch == 'e') output.push_back('=');
            else return false;
            escaped = false;
        } else output.push_back(ch);
    }
    return !escaped;
}

bool parseUnsigned(const std::string& value, std::uint64_t& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool validCatalog(const OverlayCatalog& catalog, std::string& error) {
    if (catalog.version != OverlayCatalogVersion
        || catalog.entries.size() > MaximumEntries) {
        error = "Invalid overlay catalog";
        return false;
    }
    for (const OverlayCatalogEntry& entry : catalog.entries) {
        if (!isHex(entry.profileUid, 32) || !isHex(entry.titleId, 16)
            || entry.profileName.empty() || entry.titleName.empty()) {
            error = "Invalid overlay catalog entry";
            return false;
        }
    }
    error.clear();
    return true;
}

bool writeAtomic(const std::string& path, const std::string& text, int& systemError) {
    return writeTextFileAtomic(path, text, systemError);
}

} // namespace

std::string serializeOverlayCatalog(const OverlayCatalog& catalog) {
    std::string text = "version=" + std::to_string(catalog.version) + "\n"
        + "generated_unix=" + std::to_string(catalog.generatedUnix) + "\n"
        + "entry_count=" + std::to_string(catalog.entries.size()) + "\n";
    for (std::size_t index = 0; index < catalog.entries.size(); ++index) {
        const OverlayCatalogEntry& entry = catalog.entries[index];
        const std::string prefix = "entry_" + std::to_string(index) + "_";
        text += prefix + "profile_uid=" + entry.profileUid + "\n"
            + prefix + "profile_name=" + escapeValue(entry.profileName) + "\n"
            + prefix + "title_id=" + entry.titleId + "\n"
            + prefix + "title_name=" + escapeValue(entry.titleName) + "\n";
    }
    return text;
}

bool parseOverlayCatalog(
    const std::string& text,
    OverlayCatalog& catalog,
    std::string& error) {
    catalog = OverlayCatalog{};
    bool versionSeen = false;
    bool generatedSeen = false;
    bool countSeen = false;
    std::size_t entryCount = 0;
    std::vector<bool> uidSeen;
    std::vector<bool> profileSeen;
    std::vector<bool> titleSeen;
    std::vector<bool> nameSeen;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos) {
            const std::string key = line.substr(0, separator);
            std::string value;
            if (!unescapeValue(line.substr(separator + 1), value)) {
                error = "Invalid escape sequence in the overlay catalog";
                return false;
            }
            std::uint64_t number = 0;
            if (key == "version" && parseUnsigned(value, number)) {
                catalog.version = static_cast<unsigned>(number);
                versionSeen = true;
            } else if (key == "generated_unix" && parseUnsigned(value, number)) {
                catalog.generatedUnix = number;
                generatedSeen = true;
            } else if (key == "entry_count" && parseUnsigned(value, number)) {
                if (number > MaximumEntries) {
                    error = "Overlay catalog is too large";
                    return false;
                }
                entryCount = static_cast<std::size_t>(number);
                catalog.entries.resize(entryCount);
                uidSeen.assign(entryCount, false);
                profileSeen.assign(entryCount, false);
                titleSeen.assign(entryCount, false);
                nameSeen.assign(entryCount, false);
                countSeen = true;
            } else if (key.rfind("entry_", 0) == 0 && countSeen) {
                const std::size_t fieldSeparator = key.find('_', 6);
                if (fieldSeparator != std::string::npos) {
                    std::uint64_t parsedIndex = 0;
                    if (parseUnsigned(
                            key.substr(6, fieldSeparator - 6), parsedIndex)
                        && parsedIndex < entryCount) {
                        const std::size_t index = static_cast<std::size_t>(parsedIndex);
                        const std::string field = key.substr(fieldSeparator + 1);
                        if (field == "profile_uid") {
                            catalog.entries[index].profileUid = value;
                            uidSeen[index] = true;
                        } else if (field == "profile_name") {
                            catalog.entries[index].profileName = value;
                            profileSeen[index] = true;
                        } else if (field == "title_id") {
                            catalog.entries[index].titleId = value;
                            titleSeen[index] = true;
                        } else if (field == "title_name") {
                            catalog.entries[index].titleName = value;
                            nameSeen[index] = true;
                        }
                    }
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !generatedSeen || !countSeen) {
        error = "Overlay catalog fields are missing";
        return false;
    }
    for (std::size_t index = 0; index < entryCount; ++index) {
        if (!uidSeen[index] || !profileSeen[index]
            || !titleSeen[index] || !nameSeen[index]) {
            error = "Incomplete overlay catalog entry";
            return false;
        }
    }
    return validCatalog(catalog, error);
}

bool loadOverlayCatalog(
    const std::string& path,
    OverlayCatalog& catalog,
    std::string& error) {
    std::string text;
    int readError = 0;
    if (!readTextFileRecoverable(path, text, readError, 4 * 1024 * 1024)) {
        error = readError == ENOENT
            ? "Overlay catalog has not been created yet"
            : "Overlay catalog is not readable";
        return false;
    }
    return parseOverlayCatalog(text, catalog, error);
}

bool writeOverlayCatalogAtomic(
    const std::string& path,
    const OverlayCatalog& catalog,
    int& systemError) {
    std::string error;
    if (!validCatalog(catalog, error)) {
        systemError = EINVAL;
        return false;
    }
    return writeAtomic(path, serializeOverlayCatalog(catalog), systemError);
}

bool findUniqueProfileUidForTitle(
    const OverlayCatalog& catalog,
    const std::string& titleId,
    std::string& profileUid) {
    profileUid.clear();
    for (const OverlayCatalogEntry& entry : catalog.entries) {
        if (entry.titleId != titleId) continue;
        if (profileUid.empty()) {
            profileUid = entry.profileUid;
        } else if (profileUid != entry.profileUid) {
            profileUid.clear();
            return false;
        }
    }
    return !profileUid.empty();
}

} // namespace nxsync
