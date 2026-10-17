#!/usr/bin/env bash
# Opens the launcher for the game data in the repo root (assets/, saves/, client.ini).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
game="${1:-$here}"
bin="${BOZ_BIN:-$here/build/linux-x86/bin}"
exec "$bin/boz-redux" --root "$game"
