/* Reads the parts of gamedef/ the client uses. The files are TOML written by the SDK's tools; this
 * reader handles the subset they use: [[table]] headers and `key = value` lines with strings,
 * integers (decimal or 0x) and booleans. */
#include "gamedef.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_VERSION "1.0.11"
#define SCHEMA 1

static struct gamedef_symbol *g_symbols;
static int g_symbol_count, g_symbol_capacity;
static struct gamedef_cvar *g_cvars;
static int g_cvar_count, g_cvar_capacity;
static struct gamedef_command *g_commands;
static int g_command_count, g_command_capacity;
static bool g_loaded;

static void *grow(void *array, int *capacity, int count, size_t item) {
    if (count < *capacity) {
        return array;
    }
    int next = *capacity ? *capacity * 2 : 64;
    void *grown = realloc(array, (size_t)next * item);
    if (grown) {
        memset((char *)grown + (size_t)*capacity * item, 0, (size_t)(next - *capacity) * item);
        *capacity = next;
    }
    return grown;
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

/* A quoted string value, unescaped in place; NULL when the value is not a string. */
static char *string_value(char *value) {
    if (*value != '"') {
        return NULL;
    }
    char *out = value, *in = value + 1;
    while (*in && *in != '"') {
        if (*in == '\\' && in[1]) {
            ++in;
            *out++ = *in == 'n' ? '\n' : *in == 't' ? '\t' : *in;
            ++in;
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
    return value;
}

static char *duplicate(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) {
        memcpy(copy, text, size);
    }
    return copy;
}

typedef void (*entry_fn)(const char *table, const char *key, char *value, bool first);

/* Calls fn for every key in every table; first is true for the first key after a header. */
static bool read_toml(const char *path, entry_fn fn) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return false;
    }
    char line[8192];
    char table[64] = "";
    bool first = false;
    while (fgets(line, sizeof(line), file)) {
        char *text = trim(line);
        if (!*text || *text == '#') {
            continue;
        }
        if (*text == '[') {
            bool array = text[1] == '[';
            char *start = text + (array ? 2 : 1);
            char *close = strchr(start, ']');
            if (close) {
                *close = '\0';
                snprintf(table, sizeof(table), "%s", trim(start));
                first = true;
            }
            continue;
        }
        char *equals = strchr(text, '=');
        if (!equals) {
            continue;
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);
        if (*value != '"') {
            char *comment = strchr(value, '#');
            if (comment) {
                *comment = '\0';
                value = trim(value);
            }
        }
        fn(table, key, value, first);
        first = false;
    }
    fclose(file);
    return true;
}

static struct {
    int schema;
    bool in_game;
    bool matches;
    char symbols[256];
    char console[256];
} g_manifest;

static void manifest_entry(const char *table, const char *key, char *value, bool first) {
    if (!*table && !strcmp(key, "schema")) {
        g_manifest.schema = atoi(value);
        return;
    }
    if (strcmp(table, "game") != 0) {
        return;
    }
    if (first) {
        g_manifest.in_game = false;
    }
    char *text = string_value(value);
    if (!text) {
        return;
    }
    if (!strcmp(key, "version")) {
        g_manifest.in_game = !strcmp(text, GAME_VERSION);
    } else if (g_manifest.in_game && !strcmp(key, "symbols")) {
        snprintf(g_manifest.symbols, sizeof(g_manifest.symbols), "%s", text);
        g_manifest.matches = true;
    } else if (g_manifest.in_game && !strcmp(key, "console")) {
        snprintf(g_manifest.console, sizeof(g_manifest.console), "%s", text);
    }
}

static void symbol_entry(const char *table, const char *key, char *value, bool first) {
    bool function = !strcmp(table, "function");
    if (!function && strcmp(table, "global") != 0) {
        return;
    }
    if (first) {
        struct gamedef_symbol *grown =
            grow(g_symbols, &g_symbol_capacity, g_symbol_count, sizeof(*g_symbols));
        if (!grown) {
            return;
        }
        g_symbols = grown;
        g_symbols[g_symbol_count++].function = function;
    }
    if (!g_symbol_count) {
        return;
    }
    struct gamedef_symbol *symbol = &g_symbols[g_symbol_count - 1];
    char *text = string_value(value);
    if (!strcmp(key, "name") && text) {
        symbol->name = duplicate(text);
    } else if (!strcmp(key, "offset")) {
        symbol->offset = (uint32_t)strtoul(value, NULL, 0);
    } else if (!strcmp(key, "thumb")) {
        symbol->thumb = !strcmp(value, "true");
    } else if (!strcmp(key, "type") && text) {
        symbol->type = duplicate(text);
    }
}

static void console_entry(const char *table, const char *key, char *value, bool first) {
    char *text = string_value(value);
    if (!strcmp(table, "cvar")) {
        if (first) {
            struct gamedef_cvar *grown = grow(g_cvars, &g_cvar_capacity, g_cvar_count, sizeof(*g_cvars));
            if (!grown) {
                return;
            }
            g_cvars = grown;
            ++g_cvar_count;
        }
        if (!g_cvar_count) {
            return;
        }
        struct gamedef_cvar *cvar = &g_cvars[g_cvar_count - 1];
        if (!strcmp(key, "lookup_only")) {
            cvar->lookup_only = !strcmp(value, "true");
        }
        if (!text) {
            return;
        }
        if (!strcmp(key, "name")) {
            cvar->name = duplicate(text);
        } else if (!strcmp(key, "type")) {
            cvar->type = duplicate(text);
        } else if (!strcmp(key, "default")) {
            cvar->def = duplicate(text);
        } else if (!strcmp(key, "used_by")) {
            cvar->used_by = duplicate(text);
        }
    } else if (!strcmp(table, "command")) {
        if (first) {
            struct gamedef_command *grown =
                grow(g_commands, &g_command_capacity, g_command_count, sizeof(*g_commands));
            if (!grown) {
                return;
            }
            g_commands = grown;
            ++g_command_count;
        }
        if (!g_command_count) {
            return;
        }
        struct gamedef_command *command = &g_commands[g_command_count - 1];
        if (!strcmp(key, "name") && text) {
            command->name = duplicate(text);
        } else if (!strcmp(key, "class") && text) {
            command->owner = duplicate(text);
        } else if (!strcmp(key, "id")) {
            command->id = atoi(value);
        }
    }
}

static bool load_from(const char *dir) {
    char path[1200];
    memset(&g_manifest, 0, sizeof(g_manifest));
    snprintf(path, sizeof(path), "%s/gamedef.toml", dir);
    if (!read_toml(path, manifest_entry)) {
        return false;
    }
    if (g_manifest.schema != SCHEMA || !g_manifest.matches) {
        fprintf(stderr, "[gamedef] %s: schema %d or no entry for game %s; need schema %d\n", path,
                g_manifest.schema, GAME_VERSION, SCHEMA);
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, g_manifest.symbols);
    if (!read_toml(path, symbol_entry)) {
        fprintf(stderr, "[gamedef] cannot read %s\n", path);
        return false;
    }
    if (g_manifest.console[0]) {
        snprintf(path, sizeof(path), "%s/%s", dir, g_manifest.console);
        read_toml(path, console_entry);
    }
    fprintf(stderr, "[gamedef] %s: %d symbols, %d cvars, %d commands\n", dir, g_symbol_count,
            g_cvar_count, g_command_count);
    return true;
}

bool gamedef_load(const char *root, const char *exe_dir) {
    if (g_loaded) {
        return true;
    }
    char dir[1100];
    const char *bases[] = {root && root[0] ? root : ".", exe_dir};
    for (int i = 0; i < 2; ++i) {
        if (!bases[i]) {
            continue;
        }
        snprintf(dir, sizeof(dir), "%s/gamedef", bases[i]);
        if (load_from(dir)) {
            g_loaded = true;
            return true;
        }
    }
    fprintf(stderr, "[gamedef] not found; mods cannot use game names\n");
    return false;
}

const struct gamedef_symbol *gamedef_find_symbol(const char *name) {
    for (int i = 0; name && i < g_symbol_count; ++i) {
        if (g_symbols[i].name && !strcmp(g_symbols[i].name, name)) {
            return &g_symbols[i];
        }
    }
    return NULL;
}

int gamedef_symbol_count(void) {
    return g_symbol_count;
}

const struct gamedef_symbol *gamedef_symbol_at(int index) {
    return index >= 0 && index < g_symbol_count ? &g_symbols[index] : NULL;
}

int gamedef_cvar_count(void) {
    return g_cvar_count;
}

const struct gamedef_cvar *gamedef_cvar_at(int index) {
    return index >= 0 && index < g_cvar_count ? &g_cvars[index] : NULL;
}

int gamedef_command_count(void) {
    return g_command_count;
}

const struct gamedef_command *gamedef_command_at(int index) {
    return index >= 0 && index < g_command_count ? &g_commands[index] : NULL;
}
