# The desktop runtime

Runs the Android Marmalade build of *Call of Duty: Black Ops Zombies* (1.0.11) on x86 Linux and Windows. Forked from [cod-boz-port](https://github.com/Producdevity/cod-boz-port) at commit 3b444d6003a78f08694e965509477d13a742ebf1. Upstream runs the game's ARM code natively on ARM handhelds; this fork runs it through an ARM CPU emulator.

## Build

```bash
cmake --preset linux-x86                 # configure; Unicorn and SDL2 build as subprojects
cmake --build --preset linux-x86         # loader, APK extractor, SDL2
cmake --build --preset linux-x86-tests   # everything, including tests
ctest --preset linux-x86                 # unit tests and the ARM bridge test
```

Outputs land in `build/linux-x86/bin/`: the launcher `boz-redux`, the game client `codboz_s3e_loader`, `codboz_apk_extract`, SDL2 and SDL2_mixer. The loader finds SDL2 next to itself. Tests build into `build/linux-x86/tests/`.

## Run

```bash
scripts/setup-game.sh [apk] [packs-dir] [game-dir]
scripts/run-desktop.sh [game-dir]
```

`run-desktop.sh` uses the bundled SDL2, picks Wayland when the 32-bit `libxkbcommon` is installed (X11 otherwise), and opens a resizable 1280x720 window.

## Design

- **32-bit host, shared address space.** The loader is built with `-m32`, so the game and the C runtime use the same pointers. `arm_emu.c` maps host memory into Unicorn on first access, one `/proc/self/maps` region at a time.
- **Calls into the runtime.** When guest code jumps to a host function, the fetch hits a shadow page filled with `svc` instructions. The trap handler calls the x86 function at that address with r0-r3 and stack arguments, writes r0/r1 back and returns to lr. Imports, extension tables and GL proc addresses all work this way.
- **Calls into the game.** `s3e_guest_call()` runs guest code on the calling thread's emulated CPU. Each host thread gets its own CPU; nested calls save and restore context.
- **Rendering.** The game draws into a 1280x720 framebuffer object that is blitted to the window on every swap. GLES 1 functions resolve through EGL when no `libGLESv1_CM` exists.
- **Input.** SDL keyboard and mouse state drive the s3e pointer in menus. During a Zombies match `src/native_input.c` sends mouse look, movement and actions straight to the game's player code (found through gamedef); Dead Ops Arcade uses the Xperia Play touchpads and keys. Game mode (mouse captured) switches on and off with the match. Controllers are mapped automatically.
- **Mods.** Mod folders in `<root>/mods` replace game files as they load (`s3e_file.c`) and run Lua 5.4 scripts (`lua_runtime.c`, hooks in `lua_hooks.c` on per-address emulator code hooks, the overlay in `overlay.cpp`). Game names come from `gamedef/`. Modding docs live in [boz-redux-sdk](https://github.com/ZappaVinny/boz-redux-sdk).

## Environment

| Variable | Effect |
| --- | --- |
| `BOZ_DISPLAY=WxH` | Game resolution (default 1280x720) |
| `BOZ_STRETCH=1` | Fill the window instead of keeping 16:9 |
| `BOZ_NO_SCALE=1` | Disable the scaling framebuffer |
| `BOZ_WINDOWED=0` | Start fullscreen |
| `BOZ_LOOK_SENS` | Mouse look speed (default 3.0: 0.022 degrees per count) |
| `BOZ_AIM_SENS` | Look speed multiplier while aiming (default 1.0) |
| `BOZ_INVERT_Y=1` | Invert vertical look |
| `BOZ_TOUCHPAD_CONTROLS=1` | Use the old touchpad input in Zombies too |
| `BOZ_MOUSE_SENS` | Dead Ops: mouse stick sensitivity (default 12000) |
| `BOZ_LOOK_RADIUS` | Dead Ops: look stick radius on the touchpad (max 191) |
| `BOZ_LOOK_MODE=swipe` | Dead Ops: swipe-style mouse look |
| `BOZ_TRACE_STATUS=1` | Status line every 2 s |
| `BOZ_TRACE_CALLS=N` | Log the first N runtime calls |
| `BOZ_TRACE_FILES=1` | Log every file the game opens |
| `BOZ_SOFTWARE_CURSOR=1` | Draw the crosshair pointer in menus |
| `BOZ_VSYNC=1\|0\|-1` | Vsync on, off, adaptive |
| `BOZ_FPS=N` | Frame rate limit (0: none) |
| `BOZ_RENDERER=mesa` | Windows: the bundled Mesa instead of ANGLE (with `GALLIUM_DRIVER`) |
| `BOZ_PLAYER_NAME` | The name the game uses for you (`[player] name`) |
| `BOZ_MULTIPLAYER_SERVER` | A server answering the game's online service (`[network] server`) |
| `BOZ_VOICE_CHAT=1` | Voice chat on (`[network] voice_chat`) |
| `BOZ_NETWORK_INTERFACE` | IPv4 address of the adapter co-op discovery uses, e.g. a Radmin VPN address (`[network] interface`; empty: automatic) |
| `BOZ_TRACE_NET=1` | Log sockets (bind, listen, accept, connect, errors) and co-op discovery (published and found games, the chosen interface) (`[debug] log_network`) |
| `BOZ_SWIPE_GAIN` | Dead Ops: swipe look speed |
| `BOZ_TRACE_CODE=1` | Trace guest code execution (very slow) |
| `BOZ_PERF_STATS=1` | Performance line every 2 s: frame times (average, 99th percentile, worst), host calls, hook calls and time, JIT cache flushes per frame (`[performance] stats`) |
| `BOZ_PROFILE=1` | Linux: sample where the CPU time goes (top 25 every 5 s); `=all` prints every sample (`module+offset`) and the guest PC, for offline symbolising |
| `BOZ_LAZY_GL=1` | Resolve GL functions when first called |
| `BOZ_WINDOW_TEST=1` | Linux: try the experimental 64 MB data window |
| `BOZ_SCREENSHOT_DIR`, `BOZ_SCREENSHOT_EVERY=N` | Write every Nth frame to that folder as PPM (debugging) |
| `BOZ_LAUNCHER_SCREENSHOT=file.bmp`, `BOZ_LAUNCHER_TAB=0..2`, `BOZ_LAUNCHER_PLAY=1` | Launcher: save the fourth frame (of that tab, or with PLAY's map chooser open) and quit; works offscreen (`SDL_VIDEODRIVER=offscreen`) |

Most of these are also settings in `client.ini`, which the launcher edits.

## Measuring performance

`scripts/bench.sh kino|apothicon [seconds] [runs]` starts the game without a window or sound in a throwaway game root (links to this checkout's assets, gamedef and mods; copies of saves and `client.ini`), autostarts the level with unlimited health, and prints one summary line per run from the `BOZ_PERF_STATS` lines: load time, average and 99th percentile frame time, host calls and hooks per frame, JIT flushes, emulator faults. `BOZ_BIN=<dir>` measures another build; compare builds by alternating runs, because single runs vary by about 5%. `BENCH_PROBE=<file.lua>` appends a Lua probe to a copy of the Developer mod (for example to log rounds and kills).

What made the client fast (2026-10-30, see the commit history for numbers):

- `host_code_address` caches confirmed host-code pages: the platform lookup (glibc `dladdr`, Windows `GetModuleHandleEx`) ran on every host call and took a third of the game thread.
- Linux builds Unicorn and the loader without PIC (`-fno-pic`, `-no-pie`): 32-bit PIC adds a `get_pc_thunk` to every helper the JIT calls.
- `third_party/patches/unicorn-notdirty-nonexec-regions.patch`: stores to executable regions take the fast path once the page holds no translated code (the game's data shares the image mapping with its code).
- `third_party/patches/unicorn-precise-store-faults.patch`: failed stores leave the translated block at the store, so the exit check after every guest load and store is gone.
- Hook changes made in one host call share one JIT flush; the trap reads and writes registers in batches.

## Co-op

The game's own Wi-Fi co-op works between PCs on one network, or on a virtual LAN that carries
multicast (Radmin VPN, Hamachi, ZeroTier; not Tailscale). Discovery is mDNS on UDP 5353 on the
first Ethernet or Wi-Fi adapter (the one with the default gateway first; tunnels are skipped); the
host then accepts TCP on a port that changes per room. So the firewall has to let the client in:
Windows asks the first time (allow private networks, and public ones for VPN adapters); on Linux
with ufw, for example `sudo ufw allow from 192.168.1.0/24`. Two players with one name are refused:
set a name in the launcher's Settings (`[player] name`), or leave it empty and each PC is
`Player-` and four characters of its device ID. PLAY's map
chooser opens a co-op room straight on a mod's map (every player needs the mod).
`[debug] log_network` (`BOZ_TRACE_NET`) logs sockets and discovery when two PCs do not find each
other.

## License

MIT; the upstream notice is in `LICENSE-cod-boz-port`.
