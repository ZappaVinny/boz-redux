#!/usr/bin/env bash
# Extracts game data from your own APK and fetches the data packs.
# Usage: ./setup.sh [path/to/com.activision.boz.apk] [folder with blackops_*.dz]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
apk="${1:-$here/com.activision.boz.apk}"
packs="${2:-$here}"
cdn="http://cdn-boz-android.callofduty.com/PROD/CODBOZ/1_0_9/"

[ -f "$apk" ] || { echo "Put your com.activision.boz.apk (version 1.0.11) next to setup.sh, or pass its path." >&2; exit 1; }
mkdir -p "$here/assets"
"$here/codboz_apk_extract" extract "$apk" "$here/assets" >/dev/null
for pack in blackops_etc.dz blackops_gles1.dz; do
  if [ -f "$here/assets/$pack" ]; then
    continue
  elif [ -f "$packs/$pack" ]; then
    cp "$packs/$pack" "$here/assets/$pack"
  else
    echo "Downloading $pack from Activision's CDN..."
    curl -fL -o "$here/assets/$pack.part" "$cdn$pack"
    mv "$here/assets/$pack.part" "$here/assets/$pack"
  fi
done
echo "Game data is ready. Start the game with ./run.sh"
