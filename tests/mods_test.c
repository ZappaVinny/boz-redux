#include "client_config.h"
#include "mods.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_root[] = "/tmp/mods_test.XXXXXX";

static void write_file(const char *relative, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_root, relative);
    for (char *slash = strchr(path + strlen(g_root) + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        mkdir(path, 0755);
        *slash = '/';
    }
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(text, file);
    fclose(file);
}

static bool ends_with(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    return a >= b && !strcmp(text + a - b, suffix);
}

int main(void) {
    assert(mkdtemp(g_root));
    struct mod_info mods[8];

    /* No mods folder: nothing to load. */
    assert(mods_scan(g_root, "", "", mods, 8) == 0);

    write_file("mods/developer/mod.toml", "# comment\n"
                                          "id = \"developer\"\n"
                                          "name = \"Developer\"\n"
                                          "version = \"0.1.0\"\n"
                                          "game = \"1.0.11\"\n"
                                          "[settings]\n"
                                          "name = \"ignored\"\n");
    write_file("mods/developer/assets/data-etc/Weapons_Kino.group.bin", "developer");
    write_file("mods/zeta/assets/weapons_kino.group.bin", "zeta");
    write_file("mods/zeta/assets/sounds/shot.mp3", "zeta");
    write_file("mods/alpha/assets/sounds/shot.mp3", "alpha");
    write_file("mods/stray.txt", "not a mod");

    /* Without an order, mods sort by id; manifests fill in names; folders give missing ids. */
    int count = mods_scan(g_root, "", "", mods, 8);
    assert(count == 3);
    assert(!strcmp(mods[0].id, "alpha") && !mods[0].has_manifest && !strcmp(mods[0].name, "alpha"));
    assert(!strcmp(mods[1].id, "developer") && mods[1].has_manifest);
    assert(!strcmp(mods[1].name, "Developer") && !strcmp(mods[1].version, "0.1.0"));
    assert(!strcmp(mods[1].game, "1.0.11"));
    assert(!strcmp(mods[2].id, "zeta") && mods[2].enabled);

    /* Listed ids come first in their order; unlisted ones follow; disabled ones are off. */
    count = mods_scan(g_root, " zeta , developer", "alpha", mods, 8);
    assert(count == 3);
    assert(!strcmp(mods[0].id, "zeta") && !strcmp(mods[1].id, "developer"));
    assert(!strcmp(mods[2].id, "alpha") && !mods[2].enabled);

    /* The game process indexes enabled mods; the later mod wins, matching ignores case and
     * falls back to the file name. */
    char ini[512];
    snprintf(ini, sizeof(ini), "%s/client.ini", g_root);
    FILE *file = fopen(ini, "w");
    assert(file);
    fputs("[mods]\norder = alpha, zeta, developer\ndisabled =\n", file);
    fclose(file);
    client_config_load(g_root);
    assert(!strcmp(client_config_mods_order(), "alpha, zeta, developer"));
    mods_init(g_root);
    assert(mods_enabled_count() == 3);
    assert(!strcmp(mods_enabled_at(2)->id, "developer"));
    assert(mods_enabled_at(3) == NULL);

    char path[1200];
    assert(mods_find_override("data-etc/weapons_kino.group.bin", path, sizeof(path)));
    assert(ends_with(path, "mods/developer/assets/data-etc/Weapons_Kino.group.bin"));
    assert(mods_find_override("data-etc/kino/WEAPONS_KINO.group.bin", path, sizeof(path)));
    assert(ends_with(path, "mods/developer/assets/data-etc/Weapons_Kino.group.bin"));
    assert(mods_find_override("./sounds/shot.mp3", path, sizeof(path)));
    assert(ends_with(path, "mods/zeta/assets/sounds/shot.mp3"));
    assert(!mods_find_override("sounds/other.mp3", path, sizeof(path)));
    assert(!mods_find_override("", path, sizeof(path)));

    char command[600];
    snprintf(command, sizeof(command), "rm -rf %s", g_root);
    assert(system(command) == 0);
    puts("mods_test: ok");
    return 0;
}
