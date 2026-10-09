/* Lua 5.4 runtime for code mods. One Lua state on the game thread; each enabled mod's
 * scripts/main.lua runs in its own environment (globals are per mod, the API is shared). The API
 * is documented in the SDK's docs/lua-api.md. */
#include "mod_runtime.h"
#include "native_input.h"

#include "gamedef.h"
#include "mods.h"
#include "platform/platform.h"
#include "s3e_host_internal.h"

#include "lua_runtime_internal.h"

#include "lauxlib.h"
#include <pthread.h>
#include "lua.h"
#include "lualib.h"

enum {
    MAX_CALL_ARGS = 6,
    STRING_SLOTS = 4,
    STRING_SLOT_BYTES = 1024,
    SETTING_LINE_BYTES = 4096,
};

static lua_State *L;
static bool g_started, g_active;
static pthread_t g_lua_thread;
static int g_current_mod = -1;  /* mod whose code is running, for log lines and registrations */

static const char *mod_id(int index) {
    const struct mod_info *mod = mods_enabled_at(index);
    return mod ? mod->id : "?";
}

static void log_line(int mod, const char *text) {
    fprintf(stderr, "[lua] %s: %s\n", mod_id(mod), text);
}

/* --- Calling Lua safely ------------------------------------------------------------------------ */

static int traceback(lua_State *state) {
    const char *message = lua_tostring(state, 1);
    luaL_traceback(state, state, message ? message : "(error object)", 1);
    return 1;
}

/* Calls the function under its nargs arguments; logs errors. Leaves nresults (or nils) on the stack. */
static bool protected_call(int mod, int nargs, int nresults) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    int saved = g_current_mod;
    g_current_mod = mod;
    int status = lua_pcall(L, nargs, nresults, base);
    g_current_mod = saved;
    lua_remove(L, base);
    if (status != LUA_OK) {
        log_line(mod, lua_tostring(L, -1));
        lua_pop(L, 1);
        for (int i = 0; i < nresults; ++i) {
            lua_pushnil(L);
        }
        return false;
    }
    return true;
}

/* --- Events and key bindings ------------------------------------------------------------------- */

/* Registry tables: EVENTS[name] = { {fn, mod, failed}, ... }, BINDS[key] = {fn, mod, failed}. */
static const char EVENTS_KEY[] = "boz.events";
static const char BINDS_KEY[] = "boz.binds";

static void push_registry_table(const char *key) {
    if (lua_getfield(L, LUA_REGISTRYINDEX, key) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }
}

static void push_handler(lua_State *state, int fn_index) {
    lua_createtable(state, 0, 3);
    lua_pushvalue(state, fn_index);
    lua_setfield(state, -2, "fn");
    lua_pushinteger(state, g_current_mod);
    lua_setfield(state, -2, "mod");
}

static int api_events_on(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    push_registry_table(EVENTS_KEY);
    if (lua_getfield(state, -1, name) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setfield(state, -3, name);
    }
    push_handler(state, 2);
    lua_rawseti(state, -2, (lua_Integer)lua_rawlen(state, -2) + 1);
    return 0;
}

static int api_events_off(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    push_registry_table(EVENTS_KEY);
    if (lua_getfield(state, -1, name) != LUA_TTABLE) {
        return 0;
    }
    int count = (int)lua_rawlen(state, -1);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, -1, i);
        lua_getfield(state, -1, "fn");
        bool same = lua_rawequal(state, -1, 2);
        lua_pop(state, 2);
        if (same) {
            /* table.remove(list, i) */
            for (int j = i; j < count; ++j) {
                lua_rawgeti(state, -1, j + 1);
                lua_rawseti(state, -2, j);
            }
            lua_pushnil(state);
            lua_rawseti(state, -2, count);
            break;
        }
    }
    return 0;
}

/* Calls one handler entry (at the stack top) with nargs arguments pushed by push_args. Returns
 * true when the handler returned true. Handlers that raise an error are switched off. */
static bool call_handler(void (*push_args)(void *), void *user, int nargs) {
    lua_getfield(L, -1, "failed");
    bool failed = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (failed) {
        return false;
    }
    lua_getfield(L, -1, "mod");
    int mod = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "fn");
    push_args(user);
    bool ok = protected_call(mod, nargs, 1);
    bool used = ok && lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (!ok) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "failed");
        log_line(mod, "handler switched off after the error above");
    }
    return used;
}

static bool dispatch_event(const char *name, void (*push_args)(void *), void *user, int nargs) {
    if (!L) {
        return false;
    }
    int top = lua_gettop(L);
    bool used = false;
    push_registry_table(EVENTS_KEY);
    if (lua_getfield(L, -1, name) == LUA_TTABLE) {
        /* Copy the list so handlers may register or remove handlers while it runs. */
        int count = (int)lua_rawlen(L, -1);
        lua_createtable(L, count, 0);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, -2, i);
            lua_rawseti(L, -2, i);
        }
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, -1, i);
            used |= call_handler(push_args, user, nargs);
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
    return used;
}

static int api_input_bind(lua_State *state) {
    const char *key = luaL_checkstring(state, 1);
    push_registry_table(BINDS_KEY);
    if (lua_isnoneornil(state, 2)) {
        lua_pushnil(state);
    } else {
        luaL_checktype(state, 2, LUA_TFUNCTION);
        push_handler(state, 2);
    }
    lua_setfield(state, -2, key);
    return 0;
}

/* input.send(action, down): the game action as is, without the "action" event. */
static int api_input_send(lua_State *state) {
    int action = native_input_action_from_name(luaL_checkstring(state, 1));
    if (action < 0) {
        return luaL_argerror(state, 1, "unknown action");
    }
    native_input_send((enum native_action)action, lua_isnone(state, 2) || lua_toboolean(state, 2));
    return 0;
}

/* input.down(action): whether the player holds the action's binding. */
static int api_input_down(lua_State *state) {
    int action = native_input_action_from_name(luaL_checkstring(state, 1));
    if (action < 0) {
        return luaL_argerror(state, 1, "unknown action");
    }
    lua_pushboolean(state, native_input_action_down((enum native_action)action));
    return 1;
}

static int api_input_aiming(lua_State *state) {
    lua_pushboolean(state, native_input_aiming());
    return 1;
}

static int api_input_native(lua_State *state) {
    lua_pushboolean(state, native_input_available());
    return 1;
}

static int api_input_unbind(lua_State *state) {
    lua_settop(state, 1);
    lua_pushnil(state);
    return api_input_bind(state);
}

/* --- Game definition (gamedef/ data: cvars and commands) ------------------------------------ */

static int api_gamedef_cvars(lua_State *state) {
    int count = gamedef_cvar_count();
    lua_createtable(state, count, 0);
    for (int i = 0; i < count; ++i) {
        const struct gamedef_cvar *cvar = gamedef_cvar_at(i);
        lua_createtable(state, 0, 3);
        lua_pushstring(state, cvar->name ? cvar->name : "");
        lua_setfield(state, -2, "name");
        lua_pushstring(state, cvar->type ? cvar->type : "");
        lua_setfield(state, -2, "type");
        lua_pushstring(state, cvar->def ? cvar->def : "");
        lua_setfield(state, -2, "default");
        if (cvar->used_by) {
            lua_pushstring(state, cvar->used_by);
            lua_setfield(state, -2, "used_by");
        }
        lua_pushboolean(state, cvar->lookup_only);
        lua_setfield(state, -2, "lookup_only");
        lua_rawseti(state, -2, i + 1);
    }
    return 1;
}

static int api_gamedef_commands(lua_State *state) {
    int count = gamedef_command_count();
    lua_createtable(state, count, 0);
    for (int i = 0; i < count; ++i) {
        const struct gamedef_command *command = gamedef_command_at(i);
        lua_createtable(state, 0, 2);
        lua_pushstring(state, command->name ? command->name : "");
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, command->id);
        lua_setfield(state, -2, "id");
        if (command->owner) {
            lua_pushstring(state, command->owner);
            lua_setfield(state, -2, "owner");
        }
        lua_rawseti(state, -2, i + 1);
    }
    return 1;
}

/* --- Game memory and calls --------------------------------------------------------------------- */

static uint32_t check_address(lua_State *state, int index) {
    if (lua_type(state, index) == LUA_TSTRING) {
        const char *name = lua_tostring(state, index);
        uint32_t address = game_symbol_address(name);
        if (!address) {
            luaL_error(state, "unknown game symbol %s", name);
        }
        return address;
    }
    lua_Integer value = luaL_checkinteger(state, index);
    if (value < 0x10000 || value > 0xffffffffLL) {
        luaL_error(state, "bad game address 0x%I", (LUA_INTEGER)value);
    }
    return (uint32_t)value;
}

static int api_game_symbol(lua_State *state) {
    uint32_t address = game_symbol_address(luaL_checkstring(state, 1));
    if (!address) {
        lua_pushnil(state);
    } else {
        lua_pushinteger(state, address);
    }
    return 1;
}

static int api_game_base(lua_State *state) {
    lua_pushinteger(state, (lua_Integer)game_image_base());
    return 1;
}

static int api_game_read(lua_State *state) {
    const char *type = luaL_checkstring(state, 1);
    uint32_t address = check_address(state, 2);
    const volatile void *p = (const volatile void *)(uintptr_t)address;
    if (!strcmp(type, "u8")) {
        lua_pushinteger(state, *(const volatile uint8_t *)p);
    } else if (!strcmp(type, "i8")) {
        lua_pushinteger(state, *(const volatile int8_t *)p);
    } else if (!strcmp(type, "u16")) {
        lua_pushinteger(state, *(const volatile uint16_t *)p);
    } else if (!strcmp(type, "i16")) {
        lua_pushinteger(state, *(const volatile int16_t *)p);
    } else if (!strcmp(type, "u32") || !strcmp(type, "ptr")) {
        lua_pushinteger(state, *(const volatile uint32_t *)p);
    } else if (!strcmp(type, "i32")) {
        lua_pushinteger(state, *(const volatile int32_t *)p);
    } else if (!strcmp(type, "f32")) {
        lua_pushnumber(state, *(const volatile float *)p);
    } else if (!strcmp(type, "bool")) {
        lua_pushboolean(state, *(const volatile uint8_t *)p != 0);
    } else if (!strcmp(type, "string")) {
        lua_Integer limit = luaL_optinteger(state, 3, 256);
        const char *text = (const char *)(uintptr_t)address;
        size_t length = 0;
        while ((lua_Integer)length < limit && text[length]) {
            ++length;
        }
        lua_pushlstring(state, text, length);
    } else {
        return luaL_error(state, "unknown type %s (u8 i8 u16 i16 u32 i32 f32 bool ptr string)", type);
    }
    return 1;
}

static int api_game_write(lua_State *state) {
    const char *type = luaL_checkstring(state, 1);
    uint32_t address = check_address(state, 2);
    volatile void *p = (volatile void *)(uintptr_t)address;
    if (!strcmp(type, "f32")) {
        *(volatile float *)p = (float)luaL_checknumber(state, 3);
    } else if (!strcmp(type, "bool")) {
        *(volatile uint8_t *)p = lua_toboolean(state, 3) ? 1 : 0;
    } else {
        lua_Integer value = luaL_checkinteger(state, 3);
        if (!strcmp(type, "u8") || !strcmp(type, "i8")) {
            *(volatile uint8_t *)p = (uint8_t)value;
        } else if (!strcmp(type, "u16") || !strcmp(type, "i16")) {
            *(volatile uint16_t *)p = (uint16_t)value;
        } else if (!strcmp(type, "u32") || !strcmp(type, "i32") || !strcmp(type, "ptr")) {
            *(volatile uint32_t *)p = (uint32_t)value;
        } else {
            return luaL_error(state, "unknown type %s (u8 i8 u16 i16 u32 i32 f32 bool ptr)", type);
        }
    }
    return 0;
}

static int api_game_call(lua_State *state) {
    static char strings[STRING_SLOTS][STRING_SLOT_BYTES];
    uint32_t target = check_address(state, 1);
    int argc = lua_gettop(state) - 1;
    if (argc > MAX_CALL_ARGS) {
        return luaL_error(state, "game.call takes at most %d arguments", MAX_CALL_ARGS);
    }
    uint32_t args[MAX_CALL_ARGS] = {0};
    int string_slot = 0;
    for (int i = 0; i < argc; ++i) {
        int index = i + 2;
        switch (lua_type(state, index)) {
        case LUA_TNIL:
            args[i] = 0;
            break;
        case LUA_TBOOLEAN:
            args[i] = lua_toboolean(state, index) ? 1 : 0;
            break;
        case LUA_TNUMBER:
            if (!lua_isinteger(state, index)) {
                return luaL_error(state, "argument %d: floats are not supported yet", i + 1);
            }
            args[i] = (uint32_t)lua_tointeger(state, index);
            break;
        case LUA_TSTRING:
            if (string_slot == STRING_SLOTS) {
                return luaL_error(state, "at most %d string arguments", STRING_SLOTS);
            }
            snprintf(strings[string_slot], STRING_SLOT_BYTES, "%s", lua_tostring(state, index));
            args[i] = (uint32_t)(uintptr_t)strings[string_slot++];
            break;
        default:
            return luaL_error(state, "argument %d: unsupported type %s", i + 1,
                              luaL_typename(state, index));
        }
    }
    uint32_t result = s3e_guest_call((const void *)(uintptr_t)target, MAX_CALL_ARGS, args[0],
                                     args[1], args[2], args[3], args[4], args[5]);
    lua_pushinteger(state, result);
    return 1;
}

static int api_game_alloc(lua_State *state) {
    lua_Integer size = luaL_checkinteger(state, 1);
    if (size < 1 || size > 16 * 1024 * 1024) {
        return luaL_error(state, "game.alloc: size must be 1 byte to 16 MB");
    }
    void *block = calloc(1, (size_t)size);
    if (!block) {
        return luaL_error(state, "game.alloc: out of memory");
    }
    lua_pushinteger(state, (lua_Integer)(uintptr_t)block);
    return 1;
}

static int api_game_free(lua_State *state) {
    free((void *)(uintptr_t)luaL_checkinteger(state, 1));
    return 0;
}

/* --- Settings: per-mod values kept in <saves>/mods/<id>.cfg ------------------------------------ */

/* Registry: SETTINGS[mod index] = {values = {...}, dirty = bool}. File lines: key=s:text, key=n:12.5
 * or key=b:true. */
static const char SETTINGS_KEY[] = "boz.settings";

static void settings_path(int mod, char *out, size_t out_size) {
    const char *home = getenv("HOME");
    snprintf(out, out_size, "%s/mods/%s.cfg", home && home[0] ? home : g_root, mod_id(mod));
}

static void push_mod_settings(lua_State *state, int mod) {
    push_registry_table(SETTINGS_KEY);
    if (lua_rawgeti(state, -1, mod + 1) == LUA_TTABLE) {
        lua_remove(state, -2);
        return;
    }
    lua_pop(state, 1);
    lua_createtable(state, 0, 2);
    lua_newtable(state);
    char path[1400];
    settings_path(mod, path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (file) {
        char line[SETTING_LINE_BYTES];
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *equals = strchr(line, '=');
            if (!equals || equals[1] == '\0' || equals[2] != ':') {
                continue;
            }
            *equals = '\0';
            char type = equals[1];
            const char *text = equals + 3;
            if (type == 'n') {
                lua_pushnumber(state, strtod(text, NULL));
            } else if (type == 'b') {
                lua_pushboolean(state, strcmp(text, "true") == 0);
            } else {
                lua_pushstring(state, text);
            }
            lua_setfield(state, -2, line);
        }
        fclose(file);
    }
    lua_setfield(state, -2, "values");
    lua_pushvalue(state, -1);
    lua_rawseti(state, -3, mod + 1);
    lua_remove(state, -2);
}

static int check_settings_mod(lua_State *state) {
    if (g_current_mod < 0) {
        return luaL_error(state, "settings can only be used by a mod's code");
    }
    return g_current_mod;
}

static int api_settings_get(lua_State *state) {
    int mod = check_settings_mod(state);
    const char *key = luaL_checkstring(state, 1);
    lua_settop(state, 2);  /* index 2 is the default (nil when not given) */
    push_mod_settings(state, mod);
    lua_getfield(state, -1, "values");
    if (lua_getfield(state, -1, key) == LUA_TNIL) {
        lua_pop(state, 1);
        lua_pushvalue(state, 2);
    }
    return 1;
}

static int api_settings_set(lua_State *state) {
    int mod = check_settings_mod(state);
    const char *key = luaL_checkstring(state, 1);
    for (const char *c = key; *c; ++c) {
        if (*c == '=' || *c == '\n' || *c == '\r') {
            return luaL_error(state, "settings keys cannot contain '=' or line breaks");
        }
    }
    int type = lua_type(state, 2);
    if (type != LUA_TNIL && type != LUA_TSTRING && type != LUA_TNUMBER && type != LUA_TBOOLEAN) {
        return luaL_error(state, "settings values are strings, numbers, booleans or nil");
    }
    push_mod_settings(state, mod);
    lua_getfield(state, -1, "values");
    lua_pushvalue(state, 2);
    lua_setfield(state, -2, key);
    lua_pop(state, 1);
    lua_pushboolean(state, 1);
    lua_setfield(state, -2, "dirty");
    return 0;
}

static void write_settings(lua_State *state, int mod, int table) {
    char path[1400], dir[1400];
    settings_path(mod, path, sizeof(path));
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
    FILE *file = fopen(path, "w");
    if (!file) {
        log_line(mod, "cannot save settings");
        return;
    }
    lua_getfield(state, table, "values");
    lua_pushnil(state);
    while (lua_next(state, -2)) {
        if (lua_type(state, -2) == LUA_TSTRING) {
            const char *key = lua_tostring(state, -2);
            switch (lua_type(state, -1)) {
            case LUA_TNUMBER:
                fprintf(file, "%s=n:%.17g\n", key, lua_tonumber(state, -1));
                break;
            case LUA_TBOOLEAN:
                fprintf(file, "%s=b:%s\n", key, lua_toboolean(state, -1) ? "true" : "false");
                break;
            case LUA_TSTRING:
                fprintf(file, "%s=s:%s\n", key, lua_tostring(state, -1));
                break;
            default:
                break;
            }
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    fclose(file);
}

/* Saves changed settings once per frame. */
static void settings_flush(void) {
    int top = lua_gettop(L);
    push_registry_table(SETTINGS_KEY);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_getfield(L, -1, "dirty");
        bool dirty = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (dirty) {
            lua_pushboolean(L, 0);
            lua_setfield(L, -2, "dirty");
            write_settings(L, (int)lua_tointeger(L, -2) - 1, lua_gettop(L));
        }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

static const luaL_Reg settings_api[] = {{"get", api_settings_get}, {"set", api_settings_set}, {NULL, NULL}};

/* --- assets.patch: transform game files as the game loads them -------------------------------- */

/* Registry: ASSET_PATCHES = { {name = lower-case file name, fn, mod}, ... } */
static const char ASSET_PATCHES_KEY[] = "boz.asset_patches";
static int g_asset_patch_count;

static const char *file_name_of(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
    return slash ? slash + 1 : path;
}

static void lower_name(const char *path, char *out, size_t out_size) {
    const char *name = file_name_of(path);
    size_t i = 0;
    for (; name[i] && i + 1 < out_size; ++i) {
        out[i] = (char)tolower((unsigned char)name[i]);
    }
    out[i] = '\0';
}

static int api_assets_patch(lua_State *state) {
    char name[256];
    lower_name(luaL_checkstring(state, 1), name, sizeof(name));
    luaL_checktype(state, 2, LUA_TFUNCTION);
    push_registry_table(ASSET_PATCHES_KEY);
    lua_createtable(state, 0, 3);
    lua_pushstring(state, name);
    lua_setfield(state, -2, "name");
    lua_pushvalue(state, 2);
    lua_setfield(state, -2, "fn");
    lua_pushinteger(state, g_current_mod);
    lua_setfield(state, -2, "mod");
    lua_rawseti(state, -2, (lua_Integer)lua_rawlen(state, -2) + 1);
    g_asset_patch_count++;
    return 0;
}

bool lua_runtime_has_asset_patch(const char *path) {
    if (!L || !g_asset_patch_count || !path || !pthread_equal(pthread_self(), g_lua_thread)) {
        return false;
    }
    char name[256];
    lower_name(path, name, sizeof(name));
    int top = lua_gettop(L);
    push_registry_table(ASSET_PATCHES_KEY);
    bool found = false;
    int count = (int)lua_rawlen(L, -1);
    for (int i = 1; i <= count && !found; ++i) {
        lua_rawgeti(L, -1, i);
        lua_getfield(L, -1, "name");
        found = strcmp(lua_tostring(L, -1), name) == 0;
        lua_pop(L, 2);
    }
    lua_settop(L, top);
    return found;
}

bool lua_runtime_patch_asset(const char *path, const void *data, size_t size, void **out,
                             size_t *out_size) {
    if (!lua_runtime_has_asset_patch(path)) {
        return false;
    }
    char name[256];
    lower_name(path, name, sizeof(name));
    int top = lua_gettop(L);
    lua_pushlstring(L, (const char *)data, size);
    int current = lua_gettop(L);
    push_registry_table(ASSET_PATCHES_KEY);
    int patches = lua_gettop(L);
    int count = (int)lua_rawlen(L, patches);
    bool changed = false;
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, patches, i);
        lua_getfield(L, -1, "name");
        bool match = strcmp(lua_tostring(L, -1), name) == 0;
        lua_pop(L, 1);
        if (!match) {
            lua_pop(L, 1);
            continue;
        }
        lua_getfield(L, -1, "mod");
        int mod = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "fn");
        lua_pushvalue(L, current);
        if (protected_call(mod, 1, 1) && lua_type(L, -1) == LUA_TSTRING) {
            lua_replace(L, current);
            changed = true;
        } else {
            if (lua_type(L, -1) != LUA_TNIL) {
                log_line(mod, "assets.patch: the patch function must return the file's bytes as a string");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (changed) {
        size_t length = 0;
        const char *bytes = lua_tolstring(L, current, &length);
        *out = malloc(length ? length : 1);
        if (*out) {
            memcpy(*out, bytes, length);
            *out_size = length;
            fprintf(stderr, "[lua] assets.patch: %s (%zu -> %zu bytes)\n", name, size, length);
        } else {
            changed = false;
        }
    }
    lua_settop(L, top);
    return changed;
}

static const luaL_Reg assets_api[] = {{"patch", api_assets_patch}, {NULL, NULL}};

/* --- Logging and per-mod environments ---------------------------------------------------------- */

static int api_log(lua_State *state) {
    /* Count the arguments first: in Lua 5.4 luaL_buffinit pushes a placeholder onto the stack,
     * which would otherwise be printed as an extra "userdata: 0x..." value. */
    int count = lua_gettop(state);
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    for (int i = 1; i <= count; ++i) {
        if (i > 1) {
            luaL_addchar(&buffer, ' ');
        }
        luaL_tolstring(state, i, NULL);
        luaL_addvalue(&buffer);
    }
    luaL_pushresult(&buffer);
    log_line(g_current_mod, lua_tostring(state, -1));
    return 0;
}

static void load_into_env(lua_State *state, const char *path, int env_index) {
    if (luaL_loadfilex(state, path, "t") != LUA_OK) {
        lua_error(state);
    }
    lua_pushvalue(state, env_index);
    lua_setupvalue(state, -2, 1); /* _ENV */
}

/* require(name) inside a mod: <mod>/scripts/<name with dots as slashes>.lua, run once. */
static int api_require(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    int mod = (int)lua_tointeger(state, lua_upvalueindex(1));
    lua_pushvalue(state, lua_upvalueindex(2)); /* env */
    int env = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(3)); /* loaded */
    int loaded = lua_gettop(state);
    if (lua_getfield(state, loaded, name) != LUA_TNIL) {
        return 1;
    }
    lua_pop(state, 1);
    const struct mod_info *info = mods_enabled_at(mod);
    char relative[256];
    snprintf(relative, sizeof(relative), "%s", name);
    for (char *c = relative; *c; ++c) {
        if (*c == '.') {
            *c = '/';
        }
    }
    if (strstr(relative, "..")) {
        return luaL_error(state, "bad module name %s", name);
    }
    char path[1400];
    snprintf(path, sizeof(path), "%s/scripts/%s.lua", info ? info->dir : ".", relative);
    load_into_env(state, path, env);
    lua_pushstring(state, name);
    lua_call(state, 1, 1);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_pushboolean(state, 1);
    }
    lua_pushvalue(state, -1);
    lua_setfield(state, loaded, name);
    return 1;
}

static void add_functions(const char *name, const luaL_Reg *functions) {
    lua_newtable(L);
    luaL_setfuncs(L, functions, 0);
    lua_setglobal(L, name);
}

static void open_api(void) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
    lua_pop(L, 7);
    /* os: keep the clock and time functions only. */
    lua_getglobal(L, "os");
    const char *removed[] = {"execute", "exit", "getenv", "remove", "rename", "tmpname", "setlocale"};
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); ++i) {
        lua_pushnil(L);
        lua_setfield(L, -2, removed[i]);
    }
    lua_pop(L, 1);
    /* Loading code from files or strings goes through require; dofile/loadfile would escape
     * the mod's environment. */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    lua_pushcfunction(L, api_log);
    lua_setglobal(L, "log");
    /* One table every mod sees (mods' own globals are separate), for code several mods carry,
     * like the standard lib, to coordinate. */
    lua_newtable(L);
    lua_setglobal(L, "shared");
    lua_pushcfunction(L, api_log);
    lua_setglobal(L, "print");

    static const luaL_Reg events[] = {{"on", api_events_on}, {"off", api_events_off}, {NULL, NULL}};
    static const luaL_Reg input[] = {{"bind", api_input_bind},     {"unbind", api_input_unbind},
                                     {"send", api_input_send},     {"down", api_input_down},
                                     {"aiming", api_input_aiming}, {"native", api_input_native},
                                     {NULL, NULL}};
    static const luaL_Reg gamedef[] = {{"cvars", api_gamedef_cvars},
                                       {"commands", api_gamedef_commands},
                                       {"symbol", api_game_symbol},
                                       {NULL, NULL}};
    static const luaL_Reg game[] = {{"symbol", api_game_symbol}, {"base", api_game_base},
                                    {"read", api_game_read},     {"write", api_game_write},
                                    {"call", api_game_call},     {"alloc", api_game_alloc},
                                    {"free", api_game_free},     {NULL, NULL}};
    add_functions("events", events);
    add_functions("input", input);
    add_functions("gamedef", gamedef);
    add_functions("settings", settings_api);
    add_functions("assets", assets_api);
    add_functions("game", game);
    overlay_open_lua(L);
    lua_hooks_open(L);
}

static void run_mod(int index) {
    const struct mod_info *mod = mods_enabled_at(index);
    char path[1200];
    snprintf(path, sizeof(path), "%s/scripts/main.lua", mod->dir);
    FILE *file = fopen(path, "r");
    if (!file) {
        return;
    }
    fclose(file);

    /* env = setmetatable({mod = {...}, require = ...}, {__index = _G}) */
    lua_newtable(L);
    int env = lua_gettop(L);
    lua_createtable(L, 0, 5);
    lua_pushstring(L, mod->id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, mod->name);
    lua_setfield(L, -2, "name");
    lua_pushstring(L, mod->version);
    lua_setfield(L, -2, "version");
    lua_pushstring(L, mod->dir);
    lua_setfield(L, -2, "dir");
    lua_setfield(L, env, "mod");
    lua_pushinteger(L, index);
    lua_pushvalue(L, env);
    lua_newtable(L);
    lua_pushcclosure(L, api_require, 3);
    lua_setfield(L, env, "require");
    lua_createtable(L, 0, 1);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, env);

    if (luaL_loadfilex(L, path, "t") != LUA_OK) {
        log_line(index, lua_tostring(L, -1));
        lua_settop(L, env - 1);
        return;
    }
    lua_pushvalue(L, env);
    lua_setupvalue(L, -2, 1);
    if (protected_call(index, 0, 0)) {
        fprintf(stderr, "[lua] started %s\n", mod->id);
        g_active = true;
    }
    lua_settop(L, env - 1);
}

bool lua_runtime_start(void) {
    if (g_started) {
        return g_active;
    }
    g_started = true;
    char exe[1024] = "";
    if (plat_lib_path((const void *)(uintptr_t)&lua_runtime_frame, exe, sizeof(exe))) {
        char *slash = strrchr(exe, '/');
#if defined(_WIN32)
        char *backslash = strrchr(exe, '\\');
        if (!slash || (backslash && backslash > slash)) {
            slash = backslash;
        }
#endif
        if (slash) {
            *slash = '\0';
        }
    }
    gamedef_load(g_root, exe[0] ? exe : NULL);
    int count = mods_enabled_count();
    bool any_script = false;
    for (int i = 0; i < count; ++i) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/scripts/main.lua", mods_enabled_at(i)->dir);
        FILE *file = fopen(path, "r");
        if (file) {
            fclose(file);
            any_script = true;
        }
    }
    if (!any_script) {
        return false;
    }
    g_lua_thread = pthread_self();
    L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "[lua] cannot create the Lua state\n");
        return false;
    }
    open_api();
    for (int i = 0; i < count; ++i) {
        run_mod(i);
    }
    return g_active;
}

static void push_frame_args(void *user) {
    lua_pushnumber(L, *(const double *)user);
}

void lua_runtime_frame(double dt) {
    lua_runtime_start();
    if (!L) {
        return;
    }
    dispatch_event("frame", push_frame_args, &dt, 1);
    settings_flush();
}

struct key_args {
    const char *name;
    bool down, repeat;
};

static void push_key_args(void *user) {
    const struct key_args *key = user;
    lua_pushstring(L, key->name);
    lua_pushboolean(L, key->down);
    lua_pushboolean(L, key->repeat);
}

static void push_nothing(void *user) {
    (void)user;
}

bool lua_runtime_key(const char *name, bool down, bool repeat) {
    if (!L || !name || !name[0]) {
        return false;
    }
    bool used = false;
    if (down && !repeat) {
        int top = lua_gettop(L);
        push_registry_table(BINDS_KEY);
        if (lua_getfield(L, -1, name) == LUA_TTABLE) {
            call_handler(push_nothing, NULL, 0);
            used = true;
        }
        lua_settop(L, top);
    }
    struct key_args args = {name, down, repeat};
    used |= dispatch_event("key", push_key_args, &args, 3);
    return used;
}

struct action_args {
    const char *name;
    bool down;
};

static void push_action_args(void *user) {
    const struct action_args *action = user;
    lua_pushstring(L, action->name);
    lua_pushboolean(L, action->down);
}

bool lua_runtime_action(const char *name, bool down) {
    struct action_args args = {name, down};
    return dispatch_event("action", push_action_args, &args, 2);
}

int lua_runtime_current_mod(void) {
    return g_current_mod;
}

bool lua_runtime_pcall(int mod, int nargs, int nresults) {
    return protected_call(mod, nargs, nresults);
}

void lua_runtime_log(int mod, const char *text) {
    log_line(mod, text);
}

bool lua_runtime_active(void) {
    return g_active;
}
