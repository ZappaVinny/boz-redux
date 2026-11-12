#ifndef BOZ_MODS_H
#define BOZ_MODS_H

/* Mods: folders in <root>/mods/<folder>/ with a mod.toml manifest, an assets/ folder of
 * replacement files and (later) scripts/. The launcher writes the load order and the switched-off
 * mods to client.ini [mods]; the game process reads them and mounts each enabled mod's assets/
 * above the game's own files. When two mods replace the same file, the later one wins. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MODS_MAX = 128, MOD_MAPS_MAX = 8 };

/* A [[map]] entry of mod.toml: how the launcher shows one of the mod's maps. */
struct mod_map_info {
    char title[96];         /* shown name; empty: the mod's name (or the map id) */
    char description[256];
    char picture[128];      /* image file in the mod folder (PNG or JPEG), or empty */
};

struct mod_info {
    char folder[128];       /* folder name under mods/ */
    char id[64];            /* manifest id, or the folder name */
    char name[96];
    char version[32];
    char author[96];
    char game[16];          /* game version the mod targets, e.g. 1.0.11 */
    char description[512];
    char dir[1024];         /* full path of the mod folder */
    char maps[MOD_MAPS_MAX][64]; /* levels the mod adds (manifest `maps = ["name", ...]`) */
    int map_count;
    struct mod_map_info map_info[MOD_MAPS_MAX]; /* [[map]] entries, by the index in maps */
    char preview[128];      /* picture of the mod (manifest `preview`), relative to its folder */
    char boz[32];           /* [sdk] boz: the standard library version the mod was built with */
    int client_api;         /* [sdk] client_api: the client Lua API it was built for; 0 unknown */
    bool has_manifest;
    bool enabled;
};

/* Lists the mods in <root>/mods in load order: ids named in order (comma separated) first, in that
 * order, then the rest by id. Ids named in disabled are switched off. Returns the count. */
int mods_scan(const char *root, const char *order, const char *disabled, struct mod_info *mods,
              int max);

/* Reads one mod folder (its mod.toml; id and name default to the folder name). False when dir
 * is not a folder. */
bool mods_read_folder(const char *dir, struct mod_info *mod);

/* Sets one string value in a mod's saved settings (<saves>/mods/<id>.cfg, the file the Lua
 * `settings` module reads), keeping its other values. A NULL value removes the key. */
bool mods_set_setting(const char *saves, const char *mod_id, const char *key, const char *value);

/* Calls fn for each file under <mod dir>/assets with its path relative to assets/ ('/'-separated).
 * Returns the number of files. */
int mods_list_assets(const struct mod_info *mod,
                     void (*fn)(const char *relative, const char *full, void *user), void *user);

/* True when the mod was built for another client Lua API than this client's (BOZ_LUA_API): it
 * may not work. Mods without the number (older SDKs, hand-made) are not flagged. */
bool mods_api_differs(const struct mod_info *mod);

/* Game process: scans the mods using client.ini [mods] and indexes the enabled mods' assets. */
void mods_init(const char *root);

/* Game process: the replacement file for a path the game asks for, matched without case on the
 * whole path, then on the file name alone (the game's packs store bare file names). */
bool mods_find_override(const char *name, char *out, size_t out_size);

/* Game process: the enabled mods in load order. */
int mods_enabled_count(void);
const struct mod_info *mods_enabled_at(int index);

#ifdef __cplusplus
}
#endif

#endif
