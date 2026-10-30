#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
game="${1:-$here}"
shift || true
bin="${BOZ_BIN:-$here/build/linux-x86/bin}"
if [ -z "${SDL_VIDEODRIVER:-}" ]; then
  # NVIDIA has no 32-bit Wayland EGL on most distributions (EGL falls back to software
  # rendering); X11 reaches the GPU through GLX.
  nvidia_no_wayland=
  if [ -e /proc/driver/nvidia/version ] && ! ldconfig -p | grep 'libnvidia-egl-wayland.so.1 (libc6)' >/dev/null; then
    nvidia_no_wayland=1
  fi
  if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -z "$nvidia_no_wayland" ] && ldconfig -p | grep 'libxkbcommon.so.0 (libc6)' >/dev/null; then
    export SDL_VIDEODRIVER=wayland
  else
    export SDL_VIDEODRIVER=x11
  fi
fi
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
export HOME="${BOZ_SAVES:-$game/saves}"
mkdir -p "$HOME"
cd "$game"
exec "$bin/codboz_s3e_loader" --root "$game" "$@" --run "$game/assets/boz.s3e.unpacked"
