#!/usr/bin/env bash
# ANGLE (OpenGL ES on Direct3D 11, BSD licence) for the 32-bit Windows build, taken from Electron's
# win32-ia32 release, which ships Chromium's ANGLE: libEGL.dll, libGLESv2.dll, d3dcompiler_47.dll.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
version=33.2.1
sha256=3d625e4905574f382e7a06eb7c372c59cfb691939e6464e65f54b253057342b4
out="$here/build/windows-x86/angle"
archive="$here/build/downloads/electron-v$version-win32-ia32.zip"
files=(libEGL.dll libGLESv2.dll d3dcompiler_47.dll LICENSES.chromium.html)

if [ -f "$out/.version" ] && [ "$(cat "$out/.version")" = "$version" ]; then
  exit 0
fi
mkdir -p "$(dirname "$archive")" "$out"
if [ ! -f "$archive" ]; then
  curl -fL -o "$archive.part" \
    "https://github.com/electron/electron/releases/download/v$version/electron-v$version-win32-ia32.zip"
  mv "$archive.part" "$archive"
fi
echo "$sha256  $archive" | sha256sum -c --quiet
bsdtar -xf "$archive" -C "$out" "${files[@]}"
echo "$version" > "$out/.version"
echo "ANGLE from Electron $version (x86) ready in $out"
