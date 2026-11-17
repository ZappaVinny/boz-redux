/* Runs a real mod script through the Lua runtime: settings (defaults, types, saving), events, key
 * bindings and the client table. */
#include "client_config.h"
#include "test_support.h"
#include "posix_compat.h"
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

int input_test_finger(int id, const char *action, float x, float y) {
    (void)id, (void)action, (void)x, (void)y;
    return 0;
}

static char g_server[128];

const char *s3e_multiplayer_server(void) {
    return g_server;
}

void s3e_multiplayer_set_server(const char *host) {
    snprintf(g_server, sizeof(g_server), "%s", host);
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
    char root[1024];
    assert(test_temp_dir(root, sizeof(root), "lua_runtime_test"));
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
               "assert(require('lib.pack').packaged == true, 'a folder with init.lua')\n"
               "events.on('frame', function(dt) settings.set('frames', (settings.get('frames', 0)) + 1) end)\n"
               "input.bind('F5', function() settings.set('f5', true) end)\n"
               "assets.patch('Menu.BIN', function(bytes) return bytes:upper() .. '!' end)\n"
               "assert(client.version == '" BOZ_VERSION "' and client.api >= 1, 'client info')\n"
               "assert(client.arch == 'x86' or client.arch == 'x64' or client.arch == 'arm64')\n"
               "assert(type(client.platform) == 'string' and type(client.touch) == 'boolean')\n"
               "assert(network.server() == '')\n"
               "network.set_server('Play.Example.org')\n"
               "assert(network.server() == '', 'the launcher chooses the server, not a mod')\n"
               "network.check('127.0.0.1', 1, function(ok, message)\n"
               "  settings.set('check', (ok and 'up: ' or 'down: ') .. message)\n"
               "end)\n"
               "settings.set('ok', true)\n");
    snprintf(path, sizeof(path), "%s/mods/t/mod.toml", root);
    write_file(path, "id = \"t\"\n\n[network]\nconnects = [\"play.example.org\", \"127.0.0.1\"]\n");
    snprintf(path, sizeof(path), "%s/mods/t/scripts/lib", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/mods/t/scripts/lib/helper.lua", root);
    write_file(path, "return {answer = 42}\n");
    snprintf(path, sizeof(path), "%s/mods/t/scripts/lib/pack", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/mods/t/scripts/lib/pack/init.lua", root);
    write_file(path, "return {packaged = true}\n");

    client_config_load(root);
    mods_init(root);
    assert(lua_runtime_start());
    lua_runtime_frame(0.016);
    lua_runtime_frame(0.016);
    assert(lua_runtime_key("F5", true, false));
    assert(!lua_runtime_key("F6", true, false));
    lua_runtime_frame(0.016);
    /* network.check: port 1 on this machine is closed, so the check fails, on a later frame. */
    char *checked = NULL;
    for (int i = 0; i < 400 && !(checked && strstr(checked, "check=")); ++i) {
        test_sleep_ms(20);
        lua_runtime_frame(0.016);
        snprintf(path, sizeof(path), "%s/mods/t.cfg", root);
        checked = read_file(path);
    }
    assert(checked && strstr(checked, "check=s:down: "));

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
    assert(strstr(saved, "frames=n:"));
    assert(strstr(saved, "f5=b:true"));

    test_remove_tree(root);
    puts("lua_runtime_test: ok");
    return 0;
}
