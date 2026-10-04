/* Runs a real mod script through the Lua runtime: settings (defaults, types, saving), events and
 * key bindings. */
#include "client_config.h"
#include "mod_runtime.h"
#include "mods.h"

#include "lua.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char g_root[1024];

/* Pieces of the client the runtime links against but this test does not exercise. */
void overlay_open_lua(void *state) {
    (void)state;
}

void lua_hooks_open(lua_State *state) {
    (void)state;
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(text, file);
    fclose(file);
}

static char *read_file(const char *path) {
    static char buffer[4096];
    FILE *file = fopen(path, "r");
    if (!file) {
        return NULL;
    }
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[n] = '\0';
    fclose(file);
    return buffer;
}

int main(void) {
    char root[] = "/tmp/lua_runtime_test.XXXXXX";
    assert(mkdtemp(root));
    snprintf(g_root, sizeof(g_root), "%s", root);
    setenv("HOME", root, 1);
    char path[512];
    snprintf(path, sizeof(path), "%s/mods", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/mods/t", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/mods/t/scripts", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/mods/t/scripts/main.lua", root);
    write_file(path,
               "assert(settings.get('missing') == nil, 'no default gives nil')\n"
               "assert(settings.get('missing', 5) == 5, 'default')\n"
               "settings.set('n', 12.5)\n"
               "settings.set('b', true)\n"
               "settings.set('s', 'hello world')\n"
               "assert(settings.get('n') == 12.5 and settings.get('b') == true)\n"
               "local helper = require('lib.helper')\n"
               "assert(helper.answer == 42, 'require from the mod')\n"
               "events.on('frame', function(dt) settings.set('frames', (settings.get('frames', 0)) + 1) end)\n"
               "input.bind('F5', function() settings.set('f5', true) end)\n"
               "assets.patch('Menu.BIN', function(bytes) return bytes:upper() .. '!' end)\n"
               "settings.set('ok', true)\n");
    snprintf(path, sizeof(path), "%s/mods/t/scripts/lib", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/mods/t/scripts/lib/helper.lua", root);
    write_file(path, "return {answer = 42}\n");

    client_config_load(root);
    mods_init(root);
    assert(lua_runtime_start());
    lua_runtime_frame(0.016);
    lua_runtime_frame(0.016);
    assert(lua_runtime_key("F5", true, false));
    assert(!lua_runtime_key("F6", true, false));
    lua_runtime_frame(0.016);

    /* assets.patch: matched by file name without case, chained output. */
    assert(lua_runtime_has_asset_patch("data-etc/menu.bin"));
    assert(!lua_runtime_has_asset_patch("data-etc/other.bin"));
    void *patched = NULL;
    size_t patched_size = 0;
    assert(lua_runtime_patch_asset("data-etc/menu.bin", "abc", 3, &patched, &patched_size));
    assert(patched_size == 4 && memcmp(patched, "ABC!", 4) == 0);
    free(patched);
    assert(!lua_runtime_patch_asset("other.bin", "abc", 3, &patched, &patched_size));

    snprintf(path, sizeof(path), "%s/mods/t.cfg", root);
    char *saved = read_file(path);
    assert(saved);
    assert(strstr(saved, "ok=b:true"));
    assert(strstr(saved, "n=n:12.5"));
    assert(strstr(saved, "s=s:hello world"));
    assert(strstr(saved, "frames=n:3"));
    assert(strstr(saved, "f5=b:true"));

    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", root);
    assert(system(command) == 0);
    puts("lua_runtime_test: ok");
    return 0;
}
