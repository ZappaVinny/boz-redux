# Roadmap

BOZ Redux brings the original *Call of Duty: Black Ops Zombies* mobile game (Android, 2011–2016) to desktop and builds a modding ecosystem around it. You bring your own copy of the game; we provide the client, the launcher and the tools.

## Where we are

- The original game code runs on desktop Linux and Windows through a built-in ARM emulator. The full game is playable with mouse and keyboard, in a resizable or fullscreen window.
- Automated builds produce a Linux tarball and a Windows zip.
- **Mods work:** replacement files, Lua code mods with hooks, settings in the game's own pause menu, and a standard library that makes common changes one line. The Redux mod adds a field of view setting to the pause menu.
- Official repositories and releases contain no game files. SDK users may work with their own
  extracted files locally.

## How it fits together

| Layer | What it is | Examples |
| --- | --- | --- |
| **Client** | What every player needs to run the game well on a PC | Windows/Linux/macOS support, audio, performance, controls, launcher, mod loading |
| **Redux mod** | A base mod loaded automatically, built with the same tools anyone can use | FOV slider, better mouse sensitivity, extra settings |
| **Mods** | Anything the community makes | New textures, UI, weapons, gameplay changes, maps |

Gameplay changes always live in mods, never hard-coded into the client.

## Milestones

### 1. Cross-platform client
- One build system for Linux and Windows, with automated builds.
- Windows support (bundled Mesa: GPU through Direct3D 12, software fallback).
- Download a zip, run it, play.

### 2. Client essentials
- Audio and music.
- Smoother frame pacing and better performance.
- Crash fixes and crash logs.
- A settings file, rebindable controls, controller support, fullscreen toggle, higher internal resolution.

### 3. Launcher
- Built into the client.
- Set up your game files: choose your APK (it checks the version and warns about tampered copies), then get the data packs from Activision's server or import your own.
- Enable, disable and order mods; change settings; play.

### 4. Mod runtime (done)
- **Asset mods:** drop replacement files into a mod folder, and they override the originals without touching your game files; or change a file as it loads.
- **Code mods in Lua:** hook game functions, read and change game state, draw overlay windows, save settings.
- **The standard lib:** player, perks, field of view, noclip, rounds, the console, pause-menu settings and menu building blocks as plain Lua functions.
- **The game definition:** named game functions, data, events and console commands, so mods don't break when internals are mapped differently.
- **The Developer mod:** the game's hidden developer console, cheats and noclip.
- Documented in [boz-redux-sdk](https://github.com/ZappaVinny/boz-redux-sdk): a guide to making mods and references for everything above.

### 5. Native PC controls (done)
- Mouse look that feels like a modern PC shooter: raw input straight to the camera, no stick acceleration.
- Movement and actions sent straight to the game, and automatic menu and game modes (no Tab).
- Hooks for mods to watch, block and send the player's actions (`boz.input`).

### 6. Redux base mod
- Field of view (done), aim toggle or hold, and more quality-of-life settings, all in the game's own pause menu.
- The first real mod, and the reference example for mod authors.

### 7. SDK projects and asset compilers
- Deterministic project builds produce complete installable mod folders and ZIPs.
- Writable native textures, materials, models, entities, collision, sectors, portals and navigation.
- A desktop SDK manages projects, validation, builds and installation.

### 8. Existing-map editing
- A Blender add-on imports and exports map geometry, materials, entities, collision and navigation.
- Debug views expose navmeshes, collision, spawn points, paths and barriers.

### 9. Custom Zombies maps
- Generate complete native map groups and register them through reusable Lua APIs.
- Keep each map's objectives, encounters and special rules in its own mod.

### 10. Full content-creation suite
- Models, skeletons, animations, weapons, effects, audio, localization, UI and lightmaps.
- Stable project migrations, compatibility contracts and complete tutorials.

### 11. 64-bit client and macOS
- Prototype after existing-map tools mature; begin the full rewrite after the custom-map MVP.
- Preserve Lua, package, project and gamedef compatibility across the runtime change.

## Mod format

A mod is a folder in `mods/`:

    mods/my-mod/
      mod.toml      id, name, version, author, game version, description
      assets/       replacement files, using the game's own file names
      scripts/      main.lua, and the standard lib as scripts/boz

See [making mods](https://github.com/ZappaVinny/boz-redux-sdk/blob/main/docs/making-mods.md).

## Getting involved

- **Players:** testing on different hardware, especially Windows, helps the most right now.
- **Modders:** start with [making mods](https://github.com/ZappaVinny/boz-redux-sdk/blob/main/docs/making-mods.md) and the [standard library](https://github.com/ZappaVinny/boz-redux-sdk/blob/main/docs/standard-library.md).
- **Reverse engineers and modders:** the tools, the symbol database and the format documentation are in [boz-redux-sdk](https://github.com/ZappaVinny/boz-redux-sdk).

## Credits

Built on [cod-boz-port](https://github.com/Producdevity/cod-boz-port) by Producdevity, [Unicorn](https://github.com/unicorn-engine/unicorn), [SDL](https://github.com/libsdl-org/SDL), [Lua](https://www.lua.org), [Dear ImGui](https://github.com/ocornut/imgui) and [destin](https://github.com/Tatsh/destin).
