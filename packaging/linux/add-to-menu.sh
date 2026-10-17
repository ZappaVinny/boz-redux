#!/usr/bin/env bash
# Adds BOZ Redux (this folder's launcher, with its icon) to the desktop's application menu.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
cat > "$apps/boz-redux.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=BOZ Redux
GenericName=Classic Black Ops Zombies
Comment=The original mobile Black Ops Zombies, on PC
Exec="$here/boz-redux"
Path=$here
Icon=$here/boz-redux.png
Terminal=false
Categories=Game;ActionGame;
StartupWMClass=boz-redux
ENTRY
echo "Added BOZ Redux to the application menu ($apps/boz-redux.desktop)."
