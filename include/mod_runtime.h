#ifndef BOZ_MOD_RUNTIME_H
#define BOZ_MOD_RUNTIME_H

/* The code-mod runtime: generic pieces only. Game addresses by gamedef name (game_image.c), the
 * Lua runtime that runs each enabled mod's scripts/main.lua (lua_runtime.c, hooks in
 * lua_hooks.c) and the in-game overlay mods draw with (overlay.cpp). Game-specific behaviour lives
 * in the mods. Everything runs on the game thread, from eglSwapBuffers and the input pump. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Game image (game_image.c) ------------------------------------------------------------- */

void game_set_image_base(uintptr_t base);
uintptr_t game_image_base(void);
/* Address of a gamedef symbol (functions carry the Thumb bit), or 0. */
uint32_t game_symbol_address(const char *name);

/* --- Lua runtime (lua_runtime.c) ----------------------------------------------------------- */

/* Loads gamedef and runs the enabled mods' scripts/main.lua (once). True if any mod script runs. */
bool lua_runtime_start(void);
/* Runs the "frame" event and saves changed mod settings. */
void lua_runtime_frame(double dt);
/* A key went down or up (SDL scancode name, as in client.ini). Returns true if a mod used it. */
bool lua_runtime_key(const char *name, bool down, bool repeat);
/* A game action changed during a match ("shoot", "aim", ...; native_input.c). Returns true if a
 * mod handled it, so the game should not get it. */
bool lua_runtime_action(const char *name, bool down);
/* True once at least one mod script is running. */
bool lua_runtime_active(void);
/* assets.patch: true if a mod transforms this file (matched by file name, no case), on the
 * thread that runs Lua. */
bool lua_runtime_has_asset_patch(const char *name);
/* Runs the mods' patches for name over data; *out is a malloc'd result. False leaves data as is. */
bool lua_runtime_patch_asset(const char *name, const void *data, size_t size, void **out,
                             size_t *out_size);

/* --- Overlay (overlay.cpp) ------------------------------------------------------------------ */

enum overlay_event_kind {
    OVERLAY_KEY,
    OVERLAY_TEXT,
    OVERLAY_MOUSE_MOVE,
    OVERLAY_MOUSE_BUTTON,
    OVERLAY_MOUSE_WHEEL,
};

struct overlay_event {
    enum overlay_event_kind kind;
    int scancode;        /* OVERLAY_KEY: SDL scancode */
    bool down;           /* OVERLAY_KEY, OVERLAY_MOUSE_BUTTON */
    int modifiers;       /* OVERLAY_KEY: 1 ctrl, 2 shift, 4 alt, 8 super */
    const char *text;    /* OVERLAY_TEXT: UTF-8 */
    float x, y;          /* OVERLAY_MOUSE_MOVE: drawable pixels */
    int button;          /* OVERLAY_MOUSE_BUTTON: 0 left, 1 right, 2 middle */
    float wheel_x, wheel_y;
};

void overlay_event(const struct overlay_event *event);
/* True while a mod has the overlay take the mouse and keyboard from the game. */
bool overlay_capturing(void);
/* Wraps the Lua frame: begin before lua_runtime_frame, end after (draws into framebuffer 0). */
void overlay_begin_frame(int width, int height, double dt);
void overlay_end_frame(void);
/* Adds the `ui` and `overlay` modules to a Lua state. */
void overlay_open_lua(void *lua_state);

#ifdef __cplusplus
}
#endif

#endif
