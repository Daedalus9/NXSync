#include "nxsync/atomic_file.hpp"
#include "nxsync/cloud_worker_status.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace nxsync {
namespace {

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

bool unescapeValue(const std::string& value, std::string& result) {
    result.clear();
    bool escaped = false;
    for (const char ch : value) {
        if (!escaped && ch == '\\') escaped = true;
        else if (escaped) {
            if (ch == '\\') result.push_back('\\');
            else if (ch == 'n') result.push_back('\n');
            else if (ch == 'r') result.push_back('\r');
            else if (ch == 'e') result.push_back('=');
            else return false;
            escaped = false;
        } else result.push_back(ch);
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

} // namespace

std::string serializeCloudWorkerStatus(const CloudWorkerStatus& status) {
    return "version=" + std::to_string(status.version) + "\n"
        + "build_version=" + escapeValue(status.buildVersion) + "\n"
        + "state=" + escapeValue(status.state) + "\n"
        + "revision_id=" + escapeValue(status.revisionId) + "\n"
        + "message=" + escapeValue(status.message) + "\n"
        + "completed=" + std::to_string(status.completed) + "\n"
        + "failed=" + std::to_string(status.failed) + "\n"
        + "bytes_transferred=" + std::to_string(status.bytesTransferred) + "\n"
        + "total_bytes=" + std::to_string(status.totalBytes) + "\n";
}

bool parseCloudWorkerStatus(
    const std::string& text,
    CloudWorkerStatus& status,
    std::string& error) {
    status = CloudWorkerStatus{};
    bool versionSeen = false;
    bool stateSeen = false;
    bool completedSeen = false;
    bool failedSeen = false;
    bool bytesSeen = false;
    bool totalSeen = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = text.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        const std::size_t separator = line.find('=');
        if (separator != std::string::npos) {
            const std::string key = line.substr(0, separator);
            std::string value;
            if (!unescapeValue(line.substr(separator + 1), value)) {
                error = "Invalid escape sequence in worker status";
                return false;
            }
            std::uint64_t number = 0;
            if (key == "version" && parseUnsigned(value, number)) {
                status.version = static_cast<unsigned>(number);
                versionSeen = true;
            } else if (key == "build_version") status.buildVersion = value;
            else if (key == "state") {
                status.state = value;
                stateSeen = true;
            } else if (key == "revision_id") status.revisionId = value;
            else if (key == "message") status.message = value;
            else if (key == "completed" && parseUnsigned(value, number)) {
                status.completed = static_cast<std::size_t>(number);
                completedSeen = true;
            } else if (key == "failed" && parseUnsigned(value, number)) {
                status.failed = static_cast<std::size_t>(number);
                failedSeen = true;
            } else if (key == "bytes_transferred" && parseUnsigned(value, number)) {
                status.bytesTransferred = number;
                bytesSeen = true;
            } else if (key == "total_bytes" && parseUnsigned(value, number)) {
                status.totalBytes = number;
                totalSeen = true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!versionSeen || !stateSeen || !completedSeen || !failedSeen
        || !bytesSeen || !totalSeen || status.version != CloudWorkerStatusVersion
        || status.buildVersion.empty() || status.state.empty()) {
        error = "Incomplete or incompatible worker status";
        return false;
    }
    error.clear();
    return true;
}

bool loadCloudWorkerStatus(
    const std::string& path,
    CloudWorkerStatus& status,
    std::string& error) {
    std::string text;
    int readError = 0;
    if (!readTextFileRecoverable(path, text, readError, 4 * 1024 * 1024)) {
        error = "Worker status is unavailable";
        return false;
    }
    return parseCloudWorkerStatus(text, status, error);
}

} // namespace nxsync
