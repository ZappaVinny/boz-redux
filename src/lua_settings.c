/* The `settings` module: per-mod values kept in <saves>/mods/<id>.cfg. */
#include "lua_runtime_internal.h"
#include "s3e_host_internal.h"

#include "lauxlib.h"

enum { SETTING_LINE_BYTES = 4096 };

/* Registry: SETTINGS[mod index] = {values = {...}, dirty = bool}. File lines: key=s:text, key=n:12.5
 * or key=b:true. */
static const char SETTINGS_KEY[] = "boz.settings";

static void settings_path(int mod, char *out, size_t out_size) {
    const char *home = getenv("HOME");
    snprintf(out, out_size, "%s/mods/%s.cfg", home && home[0] ? home : g_root, lua_runtime_mod_id(mod));
}

static void push_mod_settings(lua_State *state, int mod) {
    lua_runtime_registry_table(state, SETTINGS_KEY);
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
    if (lua_runtime_current_mod() < 0) {
        return luaL_error(state, "settings can only be used by a mod's code");
    }
    return lua_runtime_current_mod();
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
        lua_runtime_log(mod, "cannot save settings");
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

void lua_settings_flush(lua_State *L) {
    int top = lua_gettop(L);
    lua_runtime_registry_table(L, SETTINGS_KEY);
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

void lua_settings_open(lua_State *state) {
    luaL_newlib(state, settings_api);
    lua_setglobal(state, "settings");
}
