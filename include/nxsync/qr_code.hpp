#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxsync {
struct QrCodeImage {
    int size{0};
    std::vector<std::uint8_t> modules;
};
// Raw modules; the renderer adds a four-module white quiet zone.
QrCodeImage makeLoginQrCode(const std::string& loginUrl);
}
