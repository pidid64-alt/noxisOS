#!/usr/bin/env bash
# Build the 23-fixes kernel and run the headless QEMU acceptance tests
# inside a throwaway Ubuntu container.
#
# For hosts without a native i386 toolchain (e.g. Windows + WSL): the
# container carries gcc-multilib, nasm and qemu itself, so the only host
# requirement is docker. Usage from the repo root:
#
#   tests/docker-test.sh                  # build + full test suite
#   tests/docker-test.sh t8_net_ne2000    # build + selected tests only
set -euo pipefail
cd "$(dirname "$0")/.."

image=noxis-build:latest
docker build -q -t "$image" - <<'EOF'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc-multilib binutils nasm make python3 qemu-system-x86 \
    && rm -rf /var/lib/apt/lists/*
EOF

docker run --rm -v "$PWD":/work -w /work "$image" \
    bash -c 'make -C 23-fixes os-image.bin && python3 tests/qemu_test.py "$@"' _ "$@"
