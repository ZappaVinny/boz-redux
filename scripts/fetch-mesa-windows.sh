#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
version=26.2.3
sha256=3f3613adb43cfd0f2e665ce2400b130c275f0b3317cb3a05566320a3a67589ed
out="$here/build/windows-x86/mesa"
archive="$here/build/downloads/mesa3d-$version-release-msvc.7z"
files=(opengl32.dll libgallium_wgl.dll libGLESv2.dll libGLESv1_CM.dll libEGL.dll dxil.dll d3d10warp.dll)

if [ -f "$out/.version" ] && [ "$(cat "$out/.version")" = "$version-wgl" ]; then
  exit 0
fi
mkdir -p "$(dirname "$archive")" "$out"
if [ ! -f "$archive" ]; then
  curl -fL -o "$archive.part" \
    "https://github.com/pal1000/mesa-dist-win/releases/download/$version/mesa3d-$version-release-msvc.7z"
  mv "$archive.part" "$archive"
fi
echo "$sha256  $archive" | sha256sum -c --quiet
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
bsdtar -xf "$archive" -C "$tmp" $(printf 'x86/%s ' "${files[@]}")
for f in "${files[@]}"; do cp "$tmp/x86/$f" "$out/"; done
echo "$version-wgl" > "$out/.version"
echo "Mesa $version (x86) ready in $out"
