#!/usr/bin/env bash
# CI build inside an archlinux:base-devel container (the same toolchain as local development).
# Usage: scripts/ci.sh linux|windows|linux64
# Local run: docker run --rm -v "$PWD:/src" -w /src archlinux:base-devel scripts/ci.sh linux
set -euo pipefail
target="${1:?usage: ci.sh linux|windows|linux64}"
here="$(cd "$(dirname "$0")/.." && pwd)"

case "$target" in
  linux)
    if ! grep -q '^\[multilib\]' /etc/pacman.conf; then
      printf '\n[multilib]\nInclude = /etc/pacman.d/mirrorlist\n' >> /etc/pacman.conf
    fi
    pacman -Syu --noconfirm --needed git cmake lib32-glibc lib32-gcc-libs lib32-mesa \
      lib32-libglvnd lib32-libxkbcommon lib32-libdecor lib32-wayland lib32-libx11 lib32-alsa-lib \
      wayland-protocols
    git config --global --add safe.directory '*'
    cd "$here"
    cmake --preset linux-x86
    cmake --build --preset linux-x86-tests -j"$(nproc)"
    ctest --preset linux-x86 --output-on-failure
    scripts/package-linux.sh
    ;;
  windows)
    pacman -Syu --noconfirm --needed git cmake mingw-w64-gcc libarchive zip curl
    git config --global --add safe.directory '*'
    cd "$here"
    cmake --preset windows-x86
    cmake --build --preset windows-x86 -j"$(nproc)"
    # The unit tests too; CI runs them on Windows (scripts/test-windows.sh).
    cmake --build build/windows-x86 -j"$(nproc)"
    scripts/package-windows.sh
    ;;
  windows64)
    # The 64-bit Windows client (Dynarmic). Dynarmic only uses Boost's headers, so the Linux
    # package's serve the cross build, from a folder of their own (not all of /usr/include).
    # CI runs its unit tests on Windows (scripts/test-windows.sh).
    pacman -Syu --noconfirm --needed git cmake mingw-w64-gcc boost libarchive zip curl
    git config --global --add safe.directory '*'
    cd "$here"
    mkdir -p build/boost-headers && ln -sfn /usr/include/boost build/boost-headers/boost
    cmake --preset windows-x64 -DBoost_INCLUDE_DIR="$here/build/boost-headers"
    cmake --build build/windows-x64 -j"$(nproc)"
    BOZ_ARCH=x64 scripts/package-windows.sh
    ;;
  linux64)
    # The 64-bit client (Dynarmic) with its tests, in both memory maps: the PC one and the
    # guest-base map Android uses, so Android-only memory bugs show up here. The host-function
    # signature table must match the sources it is generated from.
    pacman -Syu --noconfirm --needed git cmake ninja boost python sdl2 mesa libglvnd libxkbcommon \
      libdecor wayland libx11 alsa-lib wayland-protocols
    git config --global --add safe.directory '*'
    cd "$here"
    python3 tools/gen_host_signatures.py
    git diff --exit-code -- src/host_signatures.c ||
      { echo "src/host_signatures.c is out of date: run tools/gen_host_signatures.py" >&2; exit 1; }
    for preset in linux-x64 linux-x64-base; do
      cmake --preset "$preset"
      cmake --build --preset "$preset-tests" -j"$(nproc)"
      ctest --preset "$preset" --output-on-failure
    done
    ;;
  *)
    echo "unknown target: $target" >&2
    exit 1
    ;;
esac
