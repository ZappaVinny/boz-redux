/* Reads the real game definition (../gamedef, the submodule) the way the client does. */
#include "gamedef.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(!gamedef_find_symbol("Console_Execute"));
    assert(gamedef_load("/nonexistent", ".."));
    assert(gamedef_load("/nonexistent", ".."));  /* a second call keeps the loaded data */

    const struct gamedef_symbol *execute = gamedef_find_symbol("Console_Execute");
    assert(execute && execute->function && execute->thumb && execute->offset == 0x0d9bfc);
    const struct gamedef_symbol *console = gamedef_find_symbol("g_console");
    assert(console && !console->function && console->offset == 0x4506f8);
    assert(!gamedef_find_symbol("NoSuchSymbol"));
    assert(gamedef_symbol_count() > 200);

    bool found_fov = false;
    for (int i = 0; i < gamedef_cvar_count(); ++i) {
        const struct gamedef_cvar *cvar = gamedef_cvar_at(i);
        if (cvar->name && !strcmp(cvar->name, "SetFov")) {
            found_fov = !strcmp(cvar->type, "float") && !strcmp(cvar->def, "50.0");
        }
    }
    assert(found_fov);
    assert(gamedef_cvar_count() > 500);
    assert(gamedef_command_count() > 100);
    assert(gamedef_command_at(0)->name && !gamedef_command_at(gamedef_command_count()));
    puts("gamedef_test: ok");
    return 0;
}
