# BOZ desktop runtime

Runs the Android Marmalade build of *Call of Duty: Black Ops Zombies* (1.0.11) on x86 Linux and Windows. Forked from [cod-boz-port](https://github.com/Producdevity/cod-boz-port) at the commit in `UPSTREAM_COMMIT`. Upstream runs the game's ARM code natively on ARM handhelds; this fork runs it through an ARM CPU emulator.

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
- **Input.** SDL keyboard and mouse state drive the s3e pointer in menu mode and the Xperia Play touchpads and keys in game mode (Tab toggles). Controllers are mapped automatically.
- **Mods.** Mod folders in `<root>/mods` replace game files as they load (`s3e_file.c`) and run Lua 5.4 scripts (`lua_runtime.c`, hooks in `lua_hooks.c` on per-address emulator code hooks, the overlay in `overlay.cpp`). Game names come from `gamedef/`. Modding docs live in [boz-redux-sdk](https://github.com/ZappaVinny/boz-redux-sdk).

## Environment

| Variable | Effect |
| --- | --- |
| `BOZ_DISPLAY=WxH` | Game resolution (default 1280x720) |
| `BOZ_STRETCH=1` | Fill the window instead of keeping 16:9 |
| `BOZ_NO_SCALE=1` | Disable the scaling framebuffer |
| `BOZ_WINDOWED=0` | Start fullscreen |
| `BOZ_MOUSE_SENS` | Mouse look sensitivity (default 12000) |
| `BOZ_LOOK_RADIUS` | Look stick radius on the touchpad (max 191) |
| `BOZ_LOOK_MODE=swipe` | Swipe-style mouse look |
| `BOZ_TRACE_STATUS=1` | Status line every 2 s |
| `BOZ_TRACE_CALLS=N` | Log the first N runtime calls |
| `BOZ_TRACE_FILES=1` | Log every file the game opens |
| `BOZ_SOFTWARE_CURSOR=1` | Draw the crosshair pointer in menus |

Most of these are also settings in `client.ini`, which the launcher edits.

## License

MIT, see `LICENSE` (upstream copyright retained).
