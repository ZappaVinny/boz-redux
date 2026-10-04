#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
build="$here/build/linux-x86"
stage="$build/package/BOZ-Redux"
rm -rf "$build/package"
mkdir -p "$stage"
cp "$build/bin/boz-redux" "$build/bin/codboz_s3e_loader" "$build/bin/codboz_apk_extract" "$stage/"
strip "$stage/boz-redux" "$stage/codboz_s3e_loader" "$stage/codboz_apk_extract"
cp -P "$build/bin/"libSDL2-2.0.so* "$build/bin/"libSDL2_mixer-2.0.so* "$stage/"
cp "$here/packaging/linux/setup.sh" "$here/packaging/linux/run.sh" "$here/packaging/linux/README.txt" "$stage/"
cp "$here/packaging/THIRD-PARTY.txt" "$stage/"
mkdir -p "$stage/gamedef" && cp -r "$here/../gamedef/"*.toml "$here/../gamedef/symbols" "$here/../gamedef/console" "$here/../gamedef/reflection" "$here/../gamedef/events" "$stage/gamedef/"
(cd "$build/package" && tar -czf BOZ-Redux-linux-x86.tar.gz BOZ-Redux)
echo "$build/package/BOZ-Redux-linux-x86.tar.gz"
