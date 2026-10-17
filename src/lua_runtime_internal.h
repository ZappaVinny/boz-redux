/* Shared between the Lua runtime's source files (not part of the client's public interface). */
#ifndef BOZ_LUA_RUNTIME_INTERNAL_H
#define BOZ_LUA_RUNTIME_INTERNAL_H

#include "lua.h"

#include <stdbool.h>

/* Index of the enabled mod whose code is running (for log lines and registrations), or -1. */
int lua_runtime_current_mod(void);
/* Calls the function under its nargs arguments as mod; logs errors with a traceback. Leaves
 * nresults values (nils after an error). Returns false after an error. */
bool lua_runtime_pcall(int mod, int nargs, int nresults);
void lua_runtime_log(int mod, const char *text);

lua_State *lua_runtime_state(void);
/* True once started and called from the thread that runs Lua (the game thread). */
bool lua_runtime_on_lua_thread(void);
const char *lua_runtime_mod_id(int mod);
/* Pushes registry[key], creating the table on first use. */
void lua_runtime_registry_table(lua_State *state, const char *key);

/* Modules in their own files: lua_hooks.c, lua_settings.c, lua_assets.c. */
void lua_hooks_open(lua_State *state);
void lua_settings_open(lua_State *state);
/* Saves changed settings; the runtime calls it once per frame. */
void lua_settings_flush(lua_State *state);
void lua_assets_open(lua_State *state);

#endif
