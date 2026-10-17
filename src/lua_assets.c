/* The `assets` module: assets.patch transforms game files as the game loads them (s3e_file.c
 * serves the result). */
#include "lua_runtime_internal.h"
#include "s3e_host_internal.h"

#include "lauxlib.h"
#include "str_util.h"
#include "mod_runtime.h"

/* Registry: ASSET_PATCHES = { {name = lower-case file name, fn, mod}, ... } */
static const char ASSET_PATCHES_KEY[] = "boz.asset_patches";
static int g_asset_patch_count;

static void lower_name(const char *path, char *out, size_t out_size) {
    str_lower(out, out_size, str_base_name(path));
}

static int api_assets_patch(lua_State *state) {
    char name[256];
    lower_name(luaL_checkstring(state, 1), name, sizeof(name));
    luaL_checktype(state, 2, LUA_TFUNCTION);
    lua_runtime_registry_table(state, ASSET_PATCHES_KEY);
    lua_createtable(state, 0, 3);
    lua_pushstring(state, name);
    lua_setfield(state, -2, "name");
    lua_pushvalue(state, 2);
    lua_setfield(state, -2, "fn");
    lua_pushinteger(state, lua_runtime_current_mod());
    lua_setfield(state, -2, "mod");
    lua_rawseti(state, -2, (lua_Integer)lua_rawlen(state, -2) + 1);
    g_asset_patch_count++;
    return 0;
}

bool lua_runtime_has_asset_patch(const char *path) {
    if (!g_asset_patch_count || !path || !lua_runtime_on_lua_thread()) {
        return false;
    }
    lua_State *L = lua_runtime_state();
    char name[256];
    lower_name(path, name, sizeof(name));
    int top = lua_gettop(L);
    lua_runtime_registry_table(L, ASSET_PATCHES_KEY);
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
    lua_State *L = lua_runtime_state();
    char name[256];
    lower_name(path, name, sizeof(name));
    int top = lua_gettop(L);
    lua_pushlstring(L, (const char *)data, size);
    int current = lua_gettop(L);
    lua_runtime_registry_table(L, ASSET_PATCHES_KEY);
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
        if (lua_runtime_pcall(mod, 1, 1) && lua_type(L, -1) == LUA_TSTRING) {
            lua_replace(L, current);
            changed = true;
        } else {
            if (lua_type(L, -1) != LUA_TNIL) {
                lua_runtime_log(mod, "assets.patch: the patch function must return the file's bytes as a string");
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

void lua_assets_open(lua_State *state) {
    luaL_newlib(state, assets_api);
    lua_setglobal(state, "assets");
}
