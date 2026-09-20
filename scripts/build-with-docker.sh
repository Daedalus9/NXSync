#!/usr/bin/env sh
set -eu

IMAGE="${NXSYNC_DEVKIT_IMAGE:-devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282}"
REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MOUNT="type=bind,source=${REPO_DIR},target=/project"

if [ "${1:-}" = "--clean" ]; then
    docker run --rm --mount "$MOUNT" -w /project "$IMAGE" make clean-all
fi

docker run --rm --mount "$MOUNT" -w /project "$IMAGE" make -j2 components
docker run --rm --mount "$MOUNT" -w /project "$IMAGE" bash scripts/test-host.sh

printf '%s\n' 'NXSync components and host tests completed successfully.'

