#!/usr/bin/env bash
# Starts the game. Saves go to ./saves; settings are in ./client.ini (written on first run).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ -f "$here/assets/boz.s3e.unpacked" ] || { echo "Run ./setup.sh first." >&2; exit 1; }
if [ -z "${SDL_VIDEODRIVER:-}" ]; then
  if [ -n "${WAYLAND_DISPLAY:-}" ] && ldconfig -p | grep 'libxkbcommon.so.0 (libc6)' >/dev/null; then
    export SDL_VIDEODRIVER=wayland
  else
    export SDL_VIDEODRIVER=x11
  fi
fi
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
export HOME="${BOZ_SAVES:-$here/saves}"
mkdir -p "$HOME"
cd "$here"
exec "$here/codboz_s3e_loader" --root "$here" "$@" \
  --run "$here/assets/boz.s3e.unpacked"
