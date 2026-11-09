#!/usr/bin/env bash
# ANGLE (OpenGL ES on Direct3D 11, BSD licence) for the Windows build, taken from Electron's
# Windows release, which ships Chromium's ANGLE: libEGL.dll, libGLESv2.dll, d3dcompiler_47.dll.
# BOZ_ARCH=x64 fetches the 64-bit DLLs for the 64-bit build (default x86).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
arch="${BOZ_ARCH:-x86}"
version=33.2.1
case "$arch" in
  x86) electron_arch=ia32; sha256=3d625e4905574f382e7a06eb7c372c59cfb691939e6464e65f54b253057342b4 ;;
  x64) electron_arch=x64; sha256=c325a6d905c67107e166267813ea2151ae7fd316c0a88de0081db0b9851a9946 ;;
  *) echo "unknown BOZ_ARCH: $arch (x86 or x64)" >&2; exit 2 ;;
esac
out="$here/build/windows-$arch/angle"
archive="$here/build/downloads/electron-v$version-win32-$electron_arch.zip"
files=(libEGL.dll libGLESv2.dll d3dcompiler_47.dll LICENSES.chromium.html)

if [ -f "$out/.version" ] && [ "$(cat "$out/.version")" = "$version" ]; then
  exit 0
fi
mkdir -p "$(dirname "$archive")" "$out"
if [ ! -f "$archive" ]; then
  curl -fL -o "$archive.part" \
    "https://github.com/electron/electron/releases/download/v$version/electron-v$version-win32-$electron_arch.zip"
  mv "$archive.part" "$archive"
fi
echo "$sha256  $archive" | sha256sum -c --quiet
bsdtar -xf "$archive" -C "$out" "${files[@]}"
echo "$version" > "$out/.version"
echo "ANGLE from Electron $version ($arch) ready in $out"
