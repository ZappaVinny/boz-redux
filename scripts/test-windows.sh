#!/usr/bin/env bash
# Runs the Windows build's checks on Windows (CI's windows-test job, Git Bash): every unit test exe
# from the repo root, then starts the packaged programs the way a player does: the APK extractor on
# a small fake APK and the launcher (one screenshot, then it quits).
# Usage: scripts/test-windows.sh   (after a Windows build: build/windows-x86/tests/*.exe and
#        build/windows-x86/package/BOZ-Redux-windows-x86.zip)
# Under Wine: BOZ_RUN=wine BOZ_NO_WINDOW=1 scripts/test-windows.sh (no launcher without a display).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
run=(${BOZ_RUN:-})
py="$(command -v python || command -v python3)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failed=0
for test in build/windows-x86/tests/*_test.exe; do
    if "${run[@]}" "$test" > "$work/out.txt" 2>&1; then
        echo "ok   $(basename "$test")"
    else
        echo "FAIL $(basename "$test")"
        cat "$work/out.txt"
        failed=1
    fi
done
[ "$failed" = 0 ] || exit 1

"$py" -m zipfile -e build/windows-x86/package/BOZ-Redux-windows-x86.zip "$work/client"
client="$(dirname "$(find "$work/client" -name boz-redux.exe | head -1)")"

# The extractor: usage without arguments, and it copies stored assets out of an APK (a ZIP).
status=0
"${run[@]}" "$client/codboz_apk_extract.exe" > "$work/out.txt" 2>&1 || status=$?
if [ "$status" != 2 ] || ! grep -q usage "$work/out.txt"; then
    echo "FAIL the APK extractor does not start (exit $status)"
    cat "$work/out.txt"
    exit 1
fi
mkdir -p "$work/apk/assets"
printf 'boz\r\n' > "$work/apk/assets/check.txt"
(cd "$work/apk" && "$py" -c 'import zipfile; zipfile.ZipFile("../fake.apk", "w").write("assets/check.txt")')
"${run[@]}" "$client/codboz_apk_extract.exe" extract "$work/fake.apk" "$work/assets" \
    > "$work/out.txt" 2>&1 || true
if ! grep -q 'missing assets/boz.s3e' "$work/out.txt" || ! cmp -s "$work/apk/assets/check.txt" "$work/assets/check.txt"; then
    echo "FAIL the APK extractor did not copy a stored asset"
    cat "$work/out.txt"
    exit 1
fi
echo "ok   APK extractor"

if [ -z "${BOZ_NO_WINDOW:-}" ]; then
    shot="$work/launcher.bmp"
    command -v cygpath > /dev/null && shot="$(cygpath -w "$shot")"
    BOZ_LAUNCHER_SCREENSHOT="$shot" timeout 60 "${run[@]}" "$client/boz-redux.exe" \
        > "$work/out.txt" 2>&1 || status=$?
    if [ ! -s "$work/launcher.bmp" ]; then
        echo "FAIL the launcher did not start"
        cat "$work/out.txt"
        exit 1
    fi
    echo "ok   launcher"
fi
