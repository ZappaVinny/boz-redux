#include "game_patches.h"

#include <string.h>

/* The game builds its generic account name with a PC-relative load of s_playerNameFormat
 * ("Player-%d"): the literal at REFERENCE holds the format's offset from the load at PC. */
enum {
    PLAYER_NAME_REFERENCE_OFFSET = 0x18f74c,
    PLAYER_NAME_REFERENCE_PC_OFFSET = 0x18f60a,
    PLAYER_NAME_FORMAT_OFFSET = 0x3af131,
};

bool player_name_install(struct s3e_loaded_image *loaded, uint32_t player_name_address) {
    static const char format[] = "Player-%d";
    if (!loaded || !loaded->base || loaded->map_size < PLAYER_NAME_FORMAT_OFFSET + sizeof(format)) {
        return false;
    }

    uint8_t *reference = loaded->base + PLAYER_NAME_REFERENCE_OFFSET;
    uint32_t relative;
    memcpy(&relative, reference, sizeof(relative));
    if (relative != PLAYER_NAME_FORMAT_OFFSET - PLAYER_NAME_REFERENCE_PC_OFFSET ||
        memcmp(loaded->base + PLAYER_NAME_FORMAT_OFFSET, format, sizeof(format)) != 0) {
        return false;
    }

    uint32_t pc = (uint32_t)(uintptr_t)loaded->base + PLAYER_NAME_REFERENCE_PC_OFFSET;
    relative = player_name_address - pc;
    memcpy(reference, &relative, sizeof(relative));
    return true;
}
