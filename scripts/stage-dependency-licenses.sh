#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_root="/opt/devkitpro/portlibs/switch/licenses"
destination="$root/.artifacts/dependency-licenses"
staging="$root/.artifacts/dependency-licenses.stage.$$"

if [[ ! -d "$source_root" ]]; then
    echo "devkitPro dependency licenses are unavailable: $source_root" >&2
    exit 1
fi

rm -rf "$staging"
mkdir -p "$staging"

copy_required() {
    local source="$1"
    local name="$2"
    if [[ ! -f "$source" ]]; then
        echo "Required dependency license is unavailable: $source" >&2
        exit 1
    fi
    cp "$source" "$staging/$name"
}

for source in "$root"/LICENSES/third-party/*.txt; do
    copy_required "$source" "$(basename "$source")"
done

copy_required "$source_root/switch-bzip2/LICENSE" "bzip2-LICENSE.txt"
copy_required "$source_root/switch-freetype/FTL.TXT" "FreeType-FTL.txt"
copy_required "$source_root/switch-freetype/GPLv2.TXT" "FreeType-GPL-2.0.txt"
copy_required "$source_root/switch-freetype/LICENSE.TXT" "FreeType-LICENSE.txt"
copy_required "$source_root/switch-libpng/LICENSE" "libpng-LICENSE.txt"
copy_required "$source_root/switch-mbedtls/LICENSE" "MbedTLS-LICENSE.txt"
copy_required "$source_root/switch-sdl2_ttf/LICENSE.txt" "SDL2_ttf-Zlib.txt"
copy_required "$source_root/switch-zlib/LICENSE" "zlib-LICENSE.txt"

(
    cd "$staging"
    find . -maxdepth 1 -type f ! -name SHA256SUMS.txt -printf '%f\n' \
        | LC_ALL=C sort \
        | while IFS= read -r name; do sha256sum "$name"; done \
        > SHA256SUMS.txt
)

if [[ -e "$destination" ]]; then
    rm -rf "$destination"
fi
mv "$staging" "$destination"
echo "Staged dependency licenses in $destination"
