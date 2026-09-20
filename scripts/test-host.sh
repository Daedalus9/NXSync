#!/usr/bin/env bash
# Run against the exact release mbedTLS implementation, not a test cipher.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
python3 scripts/stage-dependency-sources.py
source_dir="$root/.artifacts/host-mbedtls-source"
build_dir="$root/.artifacts/host-mbedtls-build"
mkdir -p "$source_dir"
tar -xf .artifacts/dependency-sources/upstream/mbedtls-2.28.10.tar.bz2 \
    -C "$source_dir" --strip-components=1
cmake -S "$source_dir" -B "$build_dir" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
cmake --build "$build_dir" --parallel 2
tls_prefix="$root/.artifacts/host-mbedtls-install"
cmake --install "$build_dir" --prefix "$tls_prefix"
# Build the same pinned libcurl as the release. The base devkitPro image has
# target curl libraries only; do not accidentally link ARM libraries on the host.
curl_source="$root/.artifacts/host-curl-source"
curl_prefix="$root/.artifacts/host-curl-install"
mkdir -p "$curl_source"
if [ ! -f "$curl_prefix/lib/libcurl.a" ]; then
    tar -xf .artifacts/dependency-sources/upstream/curl-7.69.1.tar.xz \
        -C "$curl_source" --strip-components=1
    (
        cd "$curl_source"
        ./configure --prefix="$curl_prefix" --disable-shared --enable-static \
            --with-mbedtls="$tls_prefix" --without-ssl --without-zlib --without-brotli \
            --without-libidn2 --without-libpsl --without-nghttp2 --without-libssh2 \
            --without-librtmp --disable-ldap --disable-ldaps
        make -j2 -C lib
        make -C lib install
        make -C include install
    )
fi
cmake -S tests -B build-tests -DCMAKE_BUILD_TYPE=Release \
    -DCURL_INCLUDE_DIR="$curl_prefix/include" \
    -DCURL_LIBRARY_RELEASE="$curl_prefix/lib/libcurl.a" \
    -DCURL_LIBRARY="$curl_prefix/lib/libcurl.a" \
    -DNXSYNC_CURL_EXTRA_LIBRARIES="$tls_prefix/lib/libmbedtls.a;$tls_prefix/lib/libmbedx509.a;$tls_prefix/lib/libmbedcrypto.a" \
    -DMBEDTLS_INCLUDE_DIR="$source_dir/include" \
    -DMBEDCRYPTO_LIBRARY="$build_dir/library/libmbedcrypto.a"
cmake --build build-tests --parallel 2
ctest --test-dir build-tests --output-on-failure
