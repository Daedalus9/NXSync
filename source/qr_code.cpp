#include "nxsync/qr_code.hpp"
#include "qrcodegen.h"

namespace nxsync {
QrCodeImage makeLoginQrCode(const std::string& loginUrl) {
    QrCodeImage image;
    if (loginUrl.empty() || loginUrl.size() > 1024 || loginUrl.find('\0') != std::string::npos) return image;
    constexpr int MaxVersion = 26;
    std::vector<std::uint8_t> temp(qrcodegen_BUFFER_LEN_FOR_VERSION(MaxVersion));
    std::vector<std::uint8_t> encoded(temp.size());
    if (!qrcodegen_encodeText(loginUrl.c_str(), temp.data(), encoded.data(), qrcodegen_Ecc_MEDIUM,
            qrcodegen_VERSION_MIN, MaxVersion, qrcodegen_Mask_AUTO, true)) return image;
    image.size = qrcodegen_getSize(encoded.data());
    image.modules.resize(image.size * image.size);
    for (int y = 0; y < image.size; ++y)
        for (int x = 0; x < image.size; ++x)
            image.modules[y * image.size + x] = qrcodegen_getModule(encoded.data(), x, y) ? 1 : 0;
    return image;
}
}
