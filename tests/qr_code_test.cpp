#include "nxsync/qr_code.hpp"
#include <cassert>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    assert(nxsync::makeLoginQrCode("").size == 0);
    assert(nxsync::makeLoginQrCode(std::string(1025, 'x')).size == 0);
    assert(nxsync::makeLoginQrCode(std::string("a\0b", 3)).size == 0);
    const std::string prefix = "https://cloud.example.test/";
    const auto largest = nxsync::makeLoginQrCode(prefix + std::string(1024 - prefix.size(), 'a'));
    assert(largest.size > 0 && largest.size <= 121);
    const auto qr = nxsync::makeLoginQrCode("https://cloud.example.test/index.php/login/flow/synthetic-qr-test");
    assert(qr.size > 0 && qr.modules.size() == static_cast<std::size_t>(qr.size * qr.size));
    if (argc == 2) {
        // Decode this image with an independent QR reader during release QA.
        std::ofstream image(argv[1], std::ios::binary);
        const int pixels = (qr.size + 8) * 6;
        image << "P5\n" << pixels << ' ' << pixels << "\n255\n";
        for (int y = 0; y < pixels; ++y) for (int x = 0; x < pixels; ++x) {
            const int mx = x / 6 - 4, my = y / 6 - 4;
            image.put(mx >= 0 && my >= 0 && mx < qr.size && my < qr.size && qr.modules[my * qr.size + mx] ? 0 : 255);
        }
    }
}
