#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
apk="${1:-$here/original/com.activision.boz.apk}"
packs="${2:-$here/original/obb}"
game="${3:-$here}"
extract="${BOZ_BIN:-$here/build/linux-x86/bin}/codboz_apk_extract"

[ -x "$extract" ] || { echo "build first: cmake --preset linux-x86 && cmake --build --preset linux-x86" >&2; exit 1; }
[ -f "$apk" ] || { echo "APK not found: $apk" >&2; exit 1; }

mkdir -p "$game/assets"
"$extract" extract "$apk" "$game/assets" >/dev/null
cdn="$("$extract" print-cdn "$game/assets/boz.s3e.unpacked")"

for pack in blackops_etc.dz blackops_gles1.dz; do
  if [ -f "$packs/$pack" ]; then
    ln -sfn "$(realpath --relative-to="$game/assets" "$packs/$pack")" "$game/assets/$pack"
  else
    echo "downloading $pack from $cdn"
    curl -fL -o "$game/assets/$pack" "$cdn$pack"
  fi
done
echo "game data ready in $game"
