BOZ Redux for Linux (x86, 32-bit)

Requirements: 32-bit OpenGL ES/EGL drivers, e.g. on Arch: lib32-mesa lib32-libglvnd
lib32-libxkbcommon lib32-libdecor; on Debian/Ubuntu: libegl1:i386 libgles2:i386 libxkbcommon0:i386.

1. Start ./boz-redux.
2. On the Game files tab, choose your own Black Ops Zombies APK (version 1.0.11; you can also
   drag it onto the window) and click Install, then click Download to fetch the data packs from
   Activision's server (or import them from a folder).
3. Click PLAY.

Settings and key bindings are on the Settings tab (stored in client.ini). The game log is
boz-log.txt in this folder.

Controls: the mouse is captured during a match and freed in menus and when paused. In a match:
mouse to look, WASD to move, Left Shift to sprint, left/right click to shoot/aim, E or F to
use, R reload, V knife, G grenade, Q tactical, C or Space crouch (hold for prone), X fire mode,
1 switch weapon, Esc pause (Esc again resumes), Tab frees the mouse until pressed again.
F11 or Alt+Enter toggles fullscreen. Look and aiming sensitivity are on the Settings tab.

./setup.sh and ./run.sh still work from a terminal if you prefer.
