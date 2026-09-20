#pragma once

#include <cstdint>
#include <string>

namespace nxsync {

struct UltrahandNotification {
    std::string title{"NXSync"};
    std::string text;
    unsigned fontSize{22};
    unsigned durationMs{4500};
    int priority{20};
    bool showTime{false};
};

std::string serializeUltrahandNotification(
    const UltrahandNotification& notification);

bool postUltrahandNotification(
    const std::string& directory,
    const std::string& appId,
    const UltrahandNotification& notification,
    std::uint64_t uniqueId,
    int& systemError);

} // namespace nxsync
