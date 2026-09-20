#include "nxsync/cloud_index.hpp"

#include "nxsync/remote_layout.hpp"
#include "nxsync/revision_parents.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdio>

namespace nxsync {
namespace {

bool isHex(const std::string& value, const std::size_t length) {
    return value.size() == length
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buffer[7]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X", ch);
                    escaped += buffer;
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

std::string safeSegment(const std::string& value, const char* fallback) {
    const std::string sanitized = sanitizePathSegment(value, false);
    return sanitized.empty() ? fallback : sanitized;
}

std::size_t valuePosition(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    std::size_t lineStart = 0;
    while (lineStart < json.size()) {
        std::size_t position = lineStart;
        while (position < json.size()
            && (json[position] == ' ' || json[position] == '\t' || json[position] == '\r')) {
            ++position;
        }
        if (json.compare(position, marker.size(), marker) == 0) {
            position += marker.size();
            while (position < json.size()
                && std::isspace(static_cast<unsigned char>(json[position]))) {
                ++position;
            }
            if (position >= json.size() || json[position] != ':') {
                return std::string::npos;
            }
            ++position;
            while (position < json.size()
                && std::isspace(static_cast<unsigned char>(json[position]))) {
                ++position;
            }
            return position;
        }
        const std::size_t nextLine = json.find('\n', lineStart);
        if (nextLine == std::string::npos) {
            break;
        }
        lineStart = nextLine + 1;
    }
    return std::string::npos;
}

std::string jsonString(const std::string& json, const std::string& key) {
    std::size_t position = valuePosition(json, key);
    if (position == std::string::npos || position >= json.size()
        || json[position] != '"') {
        return {};
    }
    ++position;
    std::string value;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char ch = json[position];
        if (escaped) {
            switch (ch) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case '\\': value.push_back('\\'); break;
                case '"': value.push_back('"'); break;
                default: return {};
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return value;
        } else if (static_cast<unsigned char>(ch) < 0x20) {
            return {};
        } else {
            value.push_back(ch);
        }
    }
    return {};
}

bool jsonStringArray(
    const std::string& json,
    const std::string& key,
    std::vector<std::string>& values,
    bool& present) {
    values.clear();
    const std::size_t start = valuePosition(json, key);
    present = start != std::string::npos;
    if (!present) return true;
    if (start >= json.size() || json[start] != '[') return false;
    std::size_t position = start + 1;
    while (position < json.size()) {
        while (position < json.size()
            && std::isspace(static_cast<unsigned char>(json[position]))) {
            ++position;
        }
        if (position < json.size() && json[position] == ']') return true;
        if (position >= json.size() || json[position] != '"') return false;
        ++position;
        std::string value;
        bool escaped = false;
        bool closed = false;
        for (; position < json.size(); ++position) {
            const char ch = json[position];
            if (escaped) {
                if (ch == 'n') value.push_back('\n');
                else if (ch == 'r') value.push_back('\r');
                else if (ch == 't') value.push_back('\t');
                else if (ch == 'b') value.push_back('\b');
                else if (ch == 'f') value.push_back('\f');
                else if (ch == '\\' || ch == '"') value.push_back(ch);
                else return false;
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                ++position;
                closed = true;
                break;
            } else if (static_cast<unsigned char>(ch) < 0x20) {
                return false;
            } else {
                value.push_back(ch);
            }
        }
        if (!closed) return false;
        values.push_back(value);
        while (position < json.size()
            && std::isspace(static_cast<unsigned char>(json[position]))) {
            ++position;
        }
        if (position < json.size() && json[position] == ',') {
            ++position;
            continue;
        }
        if (position < json.size() && json[position] == ']') return true;
        return false;
    }
    return false;
}

std::string jsonStringArray(const std::vector<std::string>& values) {
    std::string result = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) result += ", ";
        result += "\"" + jsonEscape(values[index]) + "\"";
    }
    return result + "]";
}

bool jsonUnsigned(
    const std::string& json,
    const std::string& key,
    std::uint64_t& value) {
    const std::size_t position = valuePosition(json, key);
    if (position == std::string::npos || position >= json.size()
        || json[position] < '0' || json[position] > '9') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(json.c_str() + position, &end, 10);
    if (errno != 0 || end == json.c_str() + position) {
        return false;
    }
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    if (*end != ',' && *end != '}') {
        return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

} // namespace

std::string makeCloudIndexEntryRemotePath(
    const std::string& remoteRoot,
    const std::string& titleId,
    const std::string& deviceId,
    const std::string& profileUid) {
    return normalizeRemoteRoot(remoteRoot)
        + "/_index/titles/" + safeSegment(titleId, "UNKNOWN-TITLE")
        + "/" + safeSegment(deviceId, "NS-UNKNOWN")
        + "_" + safeSegment(profileUid, "UNKNOWN-UID") + ".json";
}

std::string makeCloudRevisionRemotePath(
    const std::string& remoteRoot,
    const std::string& titleId,
    const std::string& revisionId) {
    return normalizeRemoteRoot(remoteRoot)
        + "/_index/revisions/" + safeSegment(titleId, "UNKNOWN-TITLE")
        + "/" + safeSegment(revisionId, "UNKNOWN-REVISION") + ".json";
}

bool validateCloudIndexEntry(const CloudIndexEntry& entry, std::string& error) {
    if (!isHex(entry.titleId, 16)) {
        error = "Invalid Title ID for the global index";
        return false;
    }
    if (entry.deviceId.empty() || entry.profileName.empty()
        || !isHex(entry.profileUid, 32)) {
        error = "Invalid source for the global index";
        return false;
    }
    if (!isHex(entry.revisionId, 64)
        || !validRevisionParents(
            entry.revisionId,
            entry.parentRevisionId,
            entry.parentRevisionIds)
        || !isHex(entry.payloadSha256, 64)
        || !isHex(entry.archiveSha256, 64)
        || entry.archiveRemotePath.empty()
        || entry.archiveRemotePath.front() != '/') {
        error = "Invalid revision or archive for the global index";
        return false;
    }
    if (entry.createdUtc.empty()) {
        error = "Incomplete global index metadata";
        return false;
    }
    error.clear();
    return true;
}

std::string serializeCloudIndexEntry(const CloudIndexEntry& entry) {
    const std::vector<std::string> parents = normalizedRevisionParents(
        entry.parentRevisionId, entry.parentRevisionIds);
    const std::string parent = parents.empty()
        ? "null"
        : "\"" + jsonEscape(parents.front()) + "\"";
    return "{\n"
        "  \"schema\": \"" + std::string(CloudIndexSchema) + "\",\n"
        "  \"index_version\": " + std::to_string(CloudIndexVersion) + ",\n"
        "  \"title_id\": \"" + jsonEscape(entry.titleId) + "\",\n"
        "  \"title_name\": \"" + jsonEscape(entry.titleName) + "\",\n"
        "  \"game_version\": \"" + jsonEscape(entry.gameVersion) + "\",\n"
        "  \"device_id\": \"" + jsonEscape(entry.deviceId) + "\",\n"
        "  \"profile_name\": \"" + jsonEscape(entry.profileName) + "\",\n"
        "  \"profile_uid\": \"" + jsonEscape(entry.profileUid) + "\",\n"
        "  \"revision_id\": \"" + jsonEscape(entry.revisionId) + "\",\n"
        "  \"parent_revision_id\": " + parent + ",\n"
        "  \"parent_revision_ids\": " + jsonStringArray(parents) + ",\n"
        "  \"payload_sha256\": \"" + jsonEscape(entry.payloadSha256) + "\",\n"
        "  \"archive_sha256\": \"" + jsonEscape(entry.archiveSha256) + "\",\n"
        "  \"archive_remote_path\": \"" + jsonEscape(entry.archiveRemotePath) + "\",\n"
        "  \"created_utc\": \"" + jsonEscape(entry.createdUtc) + "\",\n"
        "  \"archive_size\": " + std::to_string(entry.archiveSize) + ",\n"
        "  \"uncompressed_bytes\": " + std::to_string(entry.uncompressedBytes) + ",\n"
        "  \"file_count\": " + std::to_string(entry.fileCount) + "\n"
        "}\n";
}

bool parseCloudIndexEntry(
    const std::string& json,
    CloudIndexEntry& entry,
    std::string& error) {
    std::uint64_t version = 0;
    if (jsonString(json, "schema") != CloudIndexSchema
        || !jsonUnsigned(json, "index_version", version)
        || version != CloudIndexVersion) {
        error = "Unsupported global index schema";
        return false;
    }
    entry = CloudIndexEntry{};
    entry.titleId = jsonString(json, "title_id");
    entry.titleName = jsonString(json, "title_name");
    entry.gameVersion = jsonString(json, "game_version");
    entry.deviceId = jsonString(json, "device_id");
    entry.profileName = jsonString(json, "profile_name");
    entry.profileUid = jsonString(json, "profile_uid");
    entry.revisionId = jsonString(json, "revision_id");
    entry.parentRevisionId = jsonString(json, "parent_revision_id");
    bool parentsPresent = false;
    if (!jsonStringArray(
            json,
            "parent_revision_ids",
            entry.parentRevisionIds,
            parentsPresent)) {
        error = "Invalid parent list in the global index";
        return false;
    }
    entry.parentRevisionIds = normalizedRevisionParents(
        entry.parentRevisionId, entry.parentRevisionIds);
    entry.parentRevisionId = entry.parentRevisionIds.empty()
        ? std::string()
        : entry.parentRevisionIds.front();
    entry.payloadSha256 = jsonString(json, "payload_sha256");
    entry.archiveSha256 = jsonString(json, "archive_sha256");
    entry.archiveRemotePath = jsonString(json, "archive_remote_path");
    entry.createdUtc = jsonString(json, "created_utc");
    jsonUnsigned(json, "archive_size", entry.archiveSize);
    jsonUnsigned(json, "uncompressed_bytes", entry.uncompressedBytes);
    std::uint64_t fileCount = 0;
    jsonUnsigned(json, "file_count", fileCount);
    entry.fileCount = static_cast<std::size_t>(fileCount);
    return validateCloudIndexEntry(entry, error);
}

} // namespace nxsync
