#ifndef BOZ_GAMEDEF_H
#define BOZ_GAMEDEF_H

/* The game definition (gamedef/, the boz-redux-gamedef submodule): named functions and globals
 * from symbols/, cvars and commands from console/. Mods use these names instead of addresses. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gamedef_symbol {
    char *name;
    uint32_t offset;   /* from the image base */
    bool function;
    bool thumb;
    char *type;        /* globals: the C type, or NULL */
};

struct gamedef_cvar {
    char *name;
    char *type;        /* int, bool, float, fixed, string, floatarray, ... */
    char *def;         /* default value as text */
    char *used_by;     /* function that registers or reads it, or NULL */
    bool lookup_only;  /* read by the game but never registered */
};

struct gamedef_command {
    char *name;
    char *owner;       /* class that registers and handles it, or NULL */
    int id;
};

/* Loads <root>/gamedef, else <dir of the executable>/gamedef. Safe to call again. */
bool gamedef_load(const char *root, const char *exe_dir);

const struct gamedef_symbol *gamedef_find_symbol(const char *name);
int gamedef_symbol_count(void);
const struct gamedef_symbol *gamedef_symbol_at(int index);
int gamedef_cvar_count(void);
const struct gamedef_cvar *gamedef_cvar_at(int index);
int gamedef_command_count(void);
const struct gamedef_command *gamedef_command_at(int index);

#ifdef __cplusplus
}
#endif

#endif
