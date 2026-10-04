/* Where the game image is loaded, and addresses of gamedef names in it. */
#include "mod_runtime.h"

#include "gamedef.h"

static uintptr_t g_image_base;

void game_set_image_base(uintptr_t base) {
    g_image_base = base;
}

uintptr_t game_image_base(void) {
    return g_image_base;
}

uint32_t game_symbol_address(const char *name) {
    const struct gamedef_symbol *symbol = gamedef_find_symbol(name);
    if (!symbol || !g_image_base) {
        return 0;
    }
    return (uint32_t)(g_image_base + symbol->offset) | (symbol->function && symbol->thumb ? 1u : 0u);
}
