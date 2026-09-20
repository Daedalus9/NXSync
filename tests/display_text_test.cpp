#include "nxsync/display_text.hpp"

#include <cassert>
#include <cstring>
#include <string>

int main() {
    const std::string pokemon = "Pok\xC3\xA9mon FireRed Version";
    const nxsync::DisplayText valid = nxsync::sanitizeDisplayUtf8(
        pokemon.c_str(), pokemon.size() + 1);
    assert(valid.value == pokemon);
    assert(valid.visibleCodepoints == 21);

    const char privateGlyph[] = {'\xEE', '\x80', '\x80', '\0'}; // U+E000
    const nxsync::DisplayText privateOnly = nxsync::sanitizeDisplayUtf8(
        privateGlyph, sizeof(privateGlyph));
    assert(privateOnly.value.empty());
    assert(privateOnly.visibleCodepoints == 0);

    const char malformed[] = {'A', static_cast<char>(0xC3), 'B', '\0'};
    const nxsync::DisplayText recovered = nxsync::sanitizeDisplayUtf8(
        malformed, sizeof(malformed));
    assert(recovered.value == "AB");
    assert(recovered.visibleCodepoints == 2);

    const char spaced[] = "  Pokemon\n\tFireRed  ";
    const nxsync::DisplayText normalized = nxsync::sanitizeDisplayUtf8(
        spaced, sizeof(spaced));
    assert(normalized.value == "Pokemon FireRed");
    return 0;
}
