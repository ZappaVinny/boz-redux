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

/* Adds the `hook` module (lua_hooks.c). */
void lua_hooks_open(lua_State *state);

#endif
