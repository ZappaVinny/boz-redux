# Classic Black Ops Zombies Mobile Redux

A desktop port and modding base for the original Android *Call of Duty: Black Ops Zombies*. It runs the game's own ARM code on x86 Linux and Windows PCs through a built-in ARM emulator, with keyboard and mouse controls and a resizable or fullscreen window.

This repo contains no game files. You need your own copy of the Android 1.0.11 APK.

See [ROADMAP.md](ROADMAP.md) for where the project is headed.

## Creation Information
This project is being done by a human guided Generative AI (LLM) system, with minimal human verification besides functionality testing. Ideally the mod tools become human workable without AI. 

## Play

1. Download a build: `BOZ-Redux-windows-x86.zip` (Windows) or `BOZ-Redux-linux-x86.tar.gz` (Linux) from the Releases page, or from the artifacts of the latest GitHub Actions run.
2. Unpack it anywhere and start `boz-redux.exe` (Windows) or `./boz-redux` (Linux). On Linux you need 32-bit OpenGL drivers (Arch: `lib32-mesa lib32-libglvnd lib32-libxkbcommon lib32-libdecor`; Debian/Ubuntu: `libegl1:i386 libgles2:i386 libxkbcommon0:i386`).
3. In the launcher's **Game files** tab, choose your own Black Ops Zombies APK (Android 1.0.11) and click **Install**, then **Download** to fetch the data packs from Activision's server (or import them from a folder).
4. Click **PLAY**. Settings and key bindings are on the **Settings** tab; the log is `boz-log.txt` next to the launcher.

### Mods

The launcher's **Mods** tab lists everything in the `mods/` folder next to it. The first mods live in [boz-redux-sdk](https://github.com/ZappaVinny/boz-redux-sdk): **Developer** (the game's hidden console, cheats, noclip) and **Redux** (a field of view setting in the pause menu). To install one, copy its folder from the SDK's `mods/` into `mods/` (each mod already contains the standard lib in `scripts/boz`).

## Building from source

### Requirements

Arch Linux with the multilib repo (For now in Proof of Concept):

```bash
sudo pacman -S --needed base-devel cmake lib32-glibc lib32-gcc-libs lib32-mesa lib32-libglvnd \
  lib32-libxkbcommon lib32-libdecor lib32-wayland lib32-libx11 lib32-alsa-lib
```

### Build and run on Linux

```bash
git submodule update --init --recursive
cd runtime
cmake --preset linux-x86                  # configure (32-bit)
cmake --build --preset linux-x86-tests     # builds Unicorn, SDL2, the loader, extractor and tests
ctest --preset linux-x86
scripts/setup-game.sh         # extracts the APK into assets/ and links the data packs
scripts/run-desktop.sh        # starts the game directly
scripts/run-launcher.sh       # or opens the launcher for the repo's game data
```

`setup-game.sh` reads `original/com.activision.boz.apk` and packs from `original/obb/` by default, and downloads any missing pack from Activision's CDN. Pass other paths as arguments: `setup-game.sh <apk> <packs-dir> <game-dir>`.

Game data goes in `assets/` and your saves in `saves/`, both at the repo root and both gitignored.

### Windows build (cross-compiled from Linux)

Needs `mingw-w64-gcc` (the POSIX-threads variant), `zip` and `bsdtar`:

```bash
cd runtime
cmake --preset windows-x86
cmake --build --preset windows-x86
scripts/package-windows.sh    # downloads ANGLE and Mesa for Windows and writes build/windows-x86/package/BOZ-Redux-windows-x86.zip
```

The Windows client renders through ANGLE (OpenGL ES on Direct3D 11, as in Chrome), which works on almost every GPU. A bundled Mesa in `mesa\` is the fallback: the launcher's Renderer setting (or `renderer` in `client.ini`) picks Mesa's Direct3D 12, Vulkan or software renderer; software is slow but always works.

## Controls

The mouse is captured while a match runs and freed in menus and when the game is paused, so there is no mode to switch. In Zombies the mouse turns the view directly (no stick acceleration) and the keys go straight to the game's actions.

| Input | Menus | During a match |
| --- | --- | --- |
| Mouse | Pointer | Look |
| Left / right click | Tap / - | Shoot / aim |
| WASD | | Move |
| Left Shift | | Sprint |
| E or F | | Use (buy, open, revive) |
| R / V | | Reload / knife |
| G / Q | | Grenade / tactical |
| C or Space | | Crouch (hold for prone) |
| X / 1 | | Fire mode / switch weapon |
| Esc | Resume (in the pause menu) | Pause |
| Tab | | Free the mouse until pressed again |
| F11 or Alt+Enter | Toggle fullscreen | Toggle fullscreen |

Settings live in `client.ini` next to the game data (the repo root when running from source), written with comments on first run: fullscreen, vsync, frame rate limit, scaling, look and aiming sensitivity, invert look. A matching `BOZ_*` environment variable overrides a setting for one run. Keyboard and mouse controls can be rebound in the launcher or the `[keys]` section; controllers are mapped automatically. Dead Ops Arcade still steers the game's touch stick with the mouse.

## Credits

The runtime is a fork of [cod-boz-port](https://github.com/Producdevity/cod-boz-port) by Producdevity (MIT). It uses [Unicorn](https://github.com/unicorn-engine/unicorn) and [SDL2](https://github.com/libsdl-org/SDL). Modding tools, reverse engineering and documentation live in [boz-redux-sdk](https://github.com/ZappaVinny/boz-redux-sdk); the game definition the client reads for mods is [boz-redux-gamedef](https://github.com/ZappaVinny/boz-redux-gamedef) (the `gamedef/` submodule).

## License

MIT, see [LICENSE](LICENSE). Release binaries include the Unicorn engine (GPLv2), so the loader binary as distributed is under the GPLv2; see `runtime/packaging/THIRD-PARTY.txt`. The forked runtime keeps its original MIT notice in [runtime/LICENSE](runtime/LICENSE). Game files belong to Activision and are not covered by this license or included in this repo.

## Contribution
Anyone is allowed to contribute, just make a PR. The project is under MIT licences, so have fun!