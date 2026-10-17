/* The touch sticks on the HUD: a PC has no touch screen, so their art in the HUD_ALL texture of
 * ingame.group.bin is made transparent wherever the group comes from (the pack, a loose file, a
 * mod's rebuilt copy). The texture is found by its resource record, not by file size. Touchpad
 * controls (BOZ_TOUCHPAD_CONTROLS) play with the sticks, so it is skipped then. */
#include "game_patches.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "str_util.h"

enum { ATLAS_WIDTH = 1024, ATLAS_HEIGHT = 512, BYTES_PER_PIXEL = 2 };

/* The end of HUD_ALL's resource record up to its texels: record size, name hash (0x619b32be),
 * then the 1024x512 RGBA4444 image header. */
static const uint8_t RECORD[] = {
    0x00, 0x20, 0x00, 0x10, 0x00, 0xbe, 0x32, 0x9b, 0x61, 0x0c, 0x43, 0x24, 0x00, 0x00,
    0x06, 0x00, 0x10, 0x00, 0x10, 0x05, 0x00, 0x10, 0x00, 0x04, 0x00, 0x02, 0x00, 0x08,
};

static const struct { uint16_t x, y, width, height; } STICKS[] = {
    {0, 0, 257, 236},
    {257, 0, 92, 72},
    {625, 431, 80, 80},
};

bool hud_sticks_apply_to(const char *name) {
    return strcasecmp(str_base_name(name), "ingame.group.bin") == 0 && !getenv("BOZ_TOUCHPAD_CONTROLS");
}

static uint8_t *find_record(uint8_t *data, size_t size) {
    for (size_t at = 0; at + sizeof(RECORD) <= size; ++at) {
        if (data[at] == RECORD[0] && memcmp(data + at, RECORD, sizeof(RECORD)) == 0) {
            return data + at;
        }
    }
    return NULL;
}

bool hud_sticks_hide(uint8_t *data, size_t size) {
    uint8_t *record = find_record(data, size);
    if (!record || (size_t)(record - data) + sizeof(RECORD) + ATLAS_WIDTH * ATLAS_HEIGHT * BYTES_PER_PIXEL > size) {
        return false;
    }
    uint8_t *pixels = record + sizeof(RECORD);
    /* RGBA4444: alpha is the low nibble of each texel's first byte. */
    for (size_t i = 0; i < sizeof(STICKS) / sizeof(STICKS[0]); ++i) {
        for (size_t y = STICKS[i].y; y < (size_t)STICKS[i].y + STICKS[i].height; ++y) {
            uint8_t *row = pixels + (y * ATLAS_WIDTH + STICKS[i].x) * BYTES_PER_PIXEL;
            for (size_t x = 0; x < STICKS[i].width; ++x) {
                row[x * BYTES_PER_PIXEL] &= 0xf0u;
            }
        }
    }
    return true;
}
