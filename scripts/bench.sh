#!/usr/bin/env bash
# Measures the client on a fixed scenario without a window or sound, in a throwaway game root
# (links to this checkout's assets, gamedef and mods; copies of saves and client.ini), so the real
# saves and settings are never touched.
# Usage: scripts/bench.sh <scenario> [seconds] [runs]
#   kino       the plain game (Developer mod only, for autostart), Kino, unlimited health
#   apothicon  the apothicon_reborn map mod (with Developer), unlimited health
# Measures [seconds] (default 60) from 15 s after the match starts, [runs] times (default 3), and
# prints one line per run plus the median run. Extra settings: BENCH_ENV="BOZ_X=1 ..."; another
# build: BOZ_BIN=<dir with codboz_s3e_loader>; keep logs: BENCH_LOGS=<dir>; a Lua probe appended
# to (a copy of) the Developer mod's main.lua: BENCH_PROBE=<file.lua>.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
scenario="${1:?usage: bench.sh kino|apothicon [seconds] [runs]}"
seconds="${2:-60}"
runs="${3:-3}"
bin="${BOZ_BIN:-$here/build/linux-x86/bin}"
settle=15

case "$scenario" in
  kino) mods=(developer); level=kino; owner=developer; vars="UnlimitedHealth=true" ;;
  apothicon) mods=(developer apothicon_reborn); level=apothicon_reborn; owner=apothicon_reborn
             vars="UnlimitedHealth=true" ;;
  *) echo "unknown scenario: $scenario" >&2; exit 1 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
logs="${BENCH_LOGS:-$work/logs}"
mkdir -p "$logs"

run_once() {
    local n="$1" root="$work/root"
    rm -rf "$root"
    mkdir -p "$root/mods"
    ln -s "$here/assets" "$root/assets"
    ln -s "$here/gamedef" "$root/gamedef"
    for mod in "${mods[@]}"; do
        if [ "$mod" = developer ] && [ -n "${BENCH_PROBE:-}" ]; then
            cp -rL "$here/mods/$mod" "$root/mods/$mod"
            cat "$BENCH_PROBE" >> "$root/mods/$mod/scripts/main.lua"
        else
            ln -s "$(readlink -f "$here/mods/$mod")" "$root/mods/$mod"
        fi
    done
    cp -r "$here/saves" "$root/saves"
    mkdir -p "$root/saves/mods"
    {
        echo "autostart_level=s:$level"
        [ -z "$vars" ] || echo "autostart_vars=s:$vars"
    } >> "$root/saves/mods/$owner.cfg"
    local order
    order="$(IFS=,; echo "${mods[*]}")"
    cat > "$root/client.ini" <<EOF
[display]
fullscreen = false
vsync = off
fps_limit = 0
resolution = 1280x720
[mods]
order = ${order//,/, }
disabled =
start_map =
[performance]
stats = true
EOF
    local log="$logs/$scenario-$n.log" total=$((settle + seconds + 120))
    # gawk's systime() stamps each line with the seconds since the run began.
    (cd "$root" && env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy HOME="$root/saves" \
        XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}" ${BENCH_ENV:-} \
        timeout "$total" "$bin/codboz_s3e_loader" --root "$root" \
        --run "$root/assets/boz.s3e.unpacked" 2>&1 | \
        gawk -v limit="$((settle + seconds))" 'BEGIN { start = systime() }
            { t = systime() - start; print t, $0; fflush() }
            /\[input\] game/ && !match_t { match_t = t }
            match_t && t > match_t + limit { exit }') > "$log" || true
    pkill -f "$root/assets/boz.s3e.unpacked" 2>/dev/null || true
    summarize "$log"
}

# One line: load time, then over the measured window the frame-weighted average frame time, the
# median of the windows' p99s, the worst frame, and per-frame counters.
summarize() {
    gawk -v settle="$settle" -v seconds="$seconds" '
        /\[input\] game/ && !match_t { match_t = $1 }
        /\[arm\]/ && !/recovered/ { faults++ }
        match_t && /\[perf\]/ && $1 >= match_t + settle && $1 <= match_t + settle + seconds {
            for (i = 1; i <= NF; i++) {
                if ($i == "frames") f = $(i + 1)
                if ($i == "avg") a = $(i + 1)
                if ($i == "p99") p[++np] = $(i + 1)
                if ($i == "worst" && $(i + 1) > worst) worst = $(i + 1)
                if ($i == "calls") { hc += $(i + 1) * f; hs += $(i + 2) * f }
                if ($i == "hooks") { hk += $(i + 1) * f; hm += $(i + 2) * f }
                if ($i == "flushes") fl += $(i + 1)
            }
            frames += f; total += a * f
        }
        END {
            if (!match_t || !frames) { print "FAILED (no match or no stats)"; exit 1 }
            n = asort(p)
            printf "load %ds avg %.2f ms (%.0f fps) p99 %.2f worst %.1f | per frame: host %.0f " \
                   "(%.2f ms) hooks %.0f (%.2f ms) | flushes %d | faults %d\n", match_t,
                   total / frames, 1000 * frames / total, p[int((n + 1) / 2)], worst, hc / frames,
                   hs / frames, hk / frames, hm / frames, fl, faults
        }' "$1"
}

results=()
for n in $(seq 1 "$runs"); do
    line="$(run_once "$n")"
    echo "run $n: $line"
    results+=("$line")
done
# The median run by average frame time.
printf '%s\n' "${results[@]}" | grep -v FAILED | sort -t' ' -k4 -g | \
    gawk '{ l[NR] = $0 } END { if (NR) print "median: " l[int((NR + 1) / 2)]; else exit 1 }'
