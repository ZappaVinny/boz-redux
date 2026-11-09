/* Where the game image is loaded, and addresses of gamedef names in it. */
#include "mod_runtime.h"

#include "gamedef.h"

static uint32_t g_image_base;

void game_set_image_base(uint32_t base) {
    g_image_base = base;
}

uint32_t game_image_base(void) {
    return g_image_base;
}

uint32_t game_symbol_address(const char *name) {
    const struct gamedef_symbol *symbol = gamedef_find_symbol(name);
    if (!symbol || !g_image_base) {
        return 0;
    }
    return (uint32_t)(g_image_base + symbol->offset) | (symbol->function && symbol->thumb ? 1u : 0u);
}

int game_code_is_thumb(uint32_t address) {
    address &= ~1u;
    if (!g_image_base || address < g_image_base) {
        return -1;
    }
    for (int i = 0; i < gamedef_symbol_count(); ++i) {
        const struct gamedef_symbol *symbol = gamedef_symbol_at(i);
        if (symbol->function && g_image_base + symbol->offset == address) {
            return symbol->thumb ? 1 : 0;
        }
    }
    return -1;
}
