#include "nxsync/ultrahand_notification.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <string>

#include <sys/stat.h>

namespace nxsync {
namespace {

std::string jsonEscape(const std::string& value) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    escaped += "\\u00";
                    escaped.push_back(Hex[(ch >> 4) & 0x0F]);
                    escaped.push_back(Hex[ch & 0x0F]);
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

bool createDirectories(const std::string& path, int& systemError) {
    if (path.empty()) {
        systemError = EINVAL;
        return false;
    }
    std::size_t start = path.find(':');
    start = start == std::string::npos ? 1 : start + 1;
    for (std::size_t index = start; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/') continue;
        const std::string partial = path.substr(0, index);
        if (partial.empty() || partial.back() == ':') continue;
        if (mkdir(partial.c_str(), 0777) != 0 && errno != EEXIST) {
            systemError = errno;
            return false;
        }
    }
    systemError = 0;
    return true;
}

std::string safeAppId(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
            result.push_back(static_cast<char>(ch));
        }
    }
    return result.empty() ? "NXSync" : result;
}

} // namespace

std::string serializeUltrahandNotification(
    const UltrahandNotification& notification) {
    const unsigned fontSize = std::max(8U, std::min(notification.fontSize, 48U));
    const unsigned duration = std::max(
        500U, std::min(notification.durationMs, 30000U));
    return "{\"title\":\"" + jsonEscape(notification.title)
        + "\",\"text\":\"" + jsonEscape(notification.text)
        + "\",\"font_size\":" + std::to_string(fontSize)
        + ",\"duration\":" + std::to_string(duration)
        + ",\"priority\":" + std::to_string(notification.priority)
        + ",\"show_time\":\""
        + (notification.showTime ? "true" : "false")
        + "\",\"alignment\":\"left\"}";
}

bool postUltrahandNotification(
    const std::string& directory,
    const std::string& appId,
    const UltrahandNotification& notification,
    const std::uint64_t uniqueId,
    int& systemError) {
    if (notification.text.empty()) {
        systemError = EINVAL;
        return false;
    }
    if (!createDirectories(directory, systemError)) return false;
    const std::string base = directory + "/" + safeAppId(appId)
        + "-" + std::to_string(uniqueId);
    const std::string temporary = base + ".pending";
    const std::string finalPath = base + ".notify";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            systemError = errno;
            return false;
        }
        output << serializeUltrahandNotification(notification);
        output.flush();
        if (!output) {
            systemError = EIO;
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), finalPath.c_str()) != 0) {
        systemError = errno;
        std::remove(temporary.c_str());
        return false;
    }
    systemError = 0;
    return true;
}

} // namespace nxsync
