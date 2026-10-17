/* Mod discovery and the asset override index. See mods.h. */
#include "mods.h"

#include "client_config.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static bool is_dir(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static void lower_copy(char *out, size_t out_size, const char *in) {
    size_t i = 0;
    for (; in[i] && i + 1 < out_size; ++i) {
        out[i] = in[i] == '\\' ? '/' : (char)tolower((unsigned char)in[i]);
    }
    out[i] = '\0';
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

/* mod.toml: top-level `key = "string"` lines are read; tables, arrays and other keys are left for
 * later readers. */
static void read_manifest(struct mod_info *mod) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/mod.toml", mod->dir);
    FILE *file = fopen(path, "r");
    if (!file) {
        return;
    }
    mod->has_manifest = true;
    char line[1024];
    bool top_level = true;
    while (fgets(line, sizeof(line), file)) {
        char *text = trim(line);
        if (*text == '[') {
            top_level = false;
        }
        char *equals = strchr(text, '=');
        if (!top_level || *text == '#' || !equals) {
            continue;
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);
        if (*value != '"') {
            continue;
        }
        char *close = strchr(value + 1, '"');
        if (!close) {
            continue;
        }
        *close = '\0';
        ++value;
        struct {
            const char *key;
            char *field;
            size_t size;
        } fields[] = {
            {"id", mod->id, sizeof(mod->id)},
            {"name", mod->name, sizeof(mod->name)},
            {"version", mod->version, sizeof(mod->version)},
            {"author", mod->author, sizeof(mod->author)},
            {"game", mod->game, sizeof(mod->game)},
            {"description", mod->description, sizeof(mod->description)},
        };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
            if (!strcmp(key, fields[i].key)) {
                snprintf(fields[i].field, fields[i].size, "%s", value);
            }
        }
    }
    fclose(file);
}

/* Position of id in a comma-separated list, or -1. */
static int list_index(const char *list, const char *id) {
    int index = 0;
    const char *item = list ? list : "";
    while (*item) {
        const char *end = strchr(item, ',');
        size_t length = end ? (size_t)(end - item) : strlen(item);
        char entry[128];
        snprintf(entry, sizeof(entry), "%.*s", (int)(length < sizeof(entry) ? length : sizeof(entry) - 1),
                 item);
        char *name = trim(entry);
        if (*name) {
            if (!strcasecmp(name, id)) {
                return index;
            }
            ++index;
        }
        if (!end) {
            break;
        }
        item = end + 1;
    }
    return -1;
}

static const char *g_sort_order;

static int compare_mods(const void *a, const void *b) {
    const struct mod_info *left = a, *right = b;
    int li = list_index(g_sort_order, left->id), ri = list_index(g_sort_order, right->id);
    if (li >= 0 || ri >= 0) {
        if (li < 0) {
            return 1;
        }
        if (ri < 0) {
            return -1;
        }
        return li - ri;
    }
    return strcasecmp(left->id, right->id);
}

int mods_scan(const char *root, const char *order, const char *disabled, struct mod_info *mods,
              int max) {
    char base[1024];
    snprintf(base, sizeof(base), "%s/mods", root && root[0] ? root : ".");
    DIR *dir = opendir(base);
    if (!dir) {
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while (count < max && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        struct mod_info *mod = &mods[count];
        memset(mod, 0, sizeof(*mod));
        snprintf(mod->dir, sizeof(mod->dir), "%s/%s", base, entry->d_name);
        if (!is_dir(mod->dir)) {
            continue;
        }
        snprintf(mod->folder, sizeof(mod->folder), "%s", entry->d_name);
        read_manifest(mod);
        if (!mod->id[0]) {
            snprintf(mod->id, sizeof(mod->id), "%s", mod->folder);
        }
        if (!mod->name[0]) {
            snprintf(mod->name, sizeof(mod->name), "%s", mod->id);
        }
        mod->enabled = list_index(disabled, mod->id) < 0;
        ++count;
    }
    closedir(dir);
    g_sort_order = order;
    qsort(mods, (size_t)count, sizeof(*mods), compare_mods);
    g_sort_order = NULL;
    return count;
}

static int walk(const char *dir, const char *relative, int depth,
                void (*fn)(const char *relative, const char *full, void *user), void *user) {
    DIR *handle = opendir(dir);
    if (!handle || depth > 16) {
        if (handle) {
            closedir(handle);
        }
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char full[1200], child[600];
        snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
        snprintf(child, sizeof(child), "%s%s%s", relative, relative[0] ? "/" : "", entry->d_name);
        if (is_dir(full)) {
            count += walk(full, child, depth + 1, fn, user);
        } else {
            fn(child, full, user);
            ++count;
        }
    }
    closedir(handle);
    return count;
}

int mods_list_assets(const struct mod_info *mod,
                     void (*fn)(const char *relative, const char *full, void *user), void *user) {
    char assets[1200];
    snprintf(assets, sizeof(assets), "%s/assets", mod->dir);
    return walk(assets, "", 0, fn, user);
}

/* Game process state. Overrides are kept in load order, so the last match wins. */

struct override {
    char *lower_path;
    const char *lower_base;
    char *full;
    int mod;
    bool logged;
};

static struct mod_info g_mods[MODS_MAX];
static const struct mod_info *g_enabled[MODS_MAX];
static int g_enabled_count;
static struct override *g_overrides;
static int g_override_count, g_override_capacity;
static int g_current_mod;

static char *duplicate(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) {
        memcpy(copy, text, size);
    }
    return copy;
}

static void add_override(const char *relative, const char *full, void *user) {
    (void)user;
    if (g_override_count == g_override_capacity) {
        int capacity = g_override_capacity ? g_override_capacity * 2 : 256;
        struct override *grown = realloc(g_overrides, (size_t)capacity * sizeof(*grown));
        if (!grown) {
            return;
        }
        g_overrides = grown;
        g_override_capacity = capacity;
    }
    char lower[600];
    lower_copy(lower, sizeof(lower), relative);
    struct override *entry = &g_overrides[g_override_count];
    entry->lower_path = duplicate(lower);
    entry->full = duplicate(full);
    if (!entry->lower_path || !entry->full) {
        free(entry->lower_path);
        free(entry->full);
        return;
    }
    const char *slash = strrchr(entry->lower_path, '/');
    entry->lower_base = slash ? slash + 1 : entry->lower_path;
    entry->mod = g_current_mod;
    entry->logged = false;
    ++g_override_count;
}

void mods_init(const char *root) {
    int count = mods_scan(root, client_config_mods_order(), client_config_mods_disabled(), g_mods,
                          MODS_MAX);
    for (int i = 0; i < count; ++i) {
        const struct mod_info *mod = &g_mods[i];
        if (!mod->enabled) {
            fprintf(stderr, "[mods] %s is switched off\n", mod->id);
            continue;
        }
        g_current_mod = i;
        g_enabled[g_enabled_count++] = mod;
        int files = mods_list_assets(mod, add_override, NULL);
        fprintf(stderr, "[mods] loaded %s %s (%d asset file%s)\n", mod->id,
                mod->version[0] ? mod->version : "", files, files == 1 ? "" : "s");
    }
}

static const struct override *find(const char *lower_name, bool by_base) {
    for (int i = g_override_count - 1; i >= 0; --i) {
        const struct override *entry = &g_overrides[i];
        if (by_base ? !strcmp(entry->lower_base, lower_name)
                    : !strcmp(entry->lower_path, lower_name)) {
            return entry;
        }
    }
    return NULL;
}

bool mods_find_override(const char *name, char *out, size_t out_size) {
    if (!g_override_count || !name || !name[0]) {
        return false;
    }
    char lower[600];
    lower_copy(lower, sizeof(lower), name[0] == '.' && name[1] == '/' ? name + 2 : name);
    const struct override *entry = find(lower, false);
    if (!entry) {
        const char *slash = strrchr(lower, '/');
        entry = find(slash ? slash + 1 : lower, true);
    }
    if (!entry) {
        return false;
    }
    if (!entry->logged) {
        ((struct override *)entry)->logged = true;
        fprintf(stderr, "[mods] %s: %s replaces %s\n", g_mods[entry->mod].id, entry->lower_path,
                name);
    }
    snprintf(out, out_size, "%s", entry->full);
    return true;
}

int mods_enabled_count(void) {
    return g_enabled_count;
}

const struct mod_info *mods_enabled_at(int index) {
    return index >= 0 && index < g_enabled_count ? g_enabled[index] : NULL;
}
