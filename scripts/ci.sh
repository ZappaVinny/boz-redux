#!/usr/bin/env bash
# CI build inside an archlinux:base-devel container (the same toolchain as local development).
# Usage: scripts/ci.sh linux|windows
# Local run: docker run --rm -v "$PWD:/src" -w /src archlinux:base-devel scripts/ci.sh linux
set -euo pipefail
target="${1:?usage: ci.sh linux|windows}"
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
    scripts/package-windows.sh
    ;;
  *)
    echo "unknown target: $target" >&2
    exit 1
    ;;
esac
