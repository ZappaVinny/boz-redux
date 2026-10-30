/* The HUD's touch stick art: found by its resource record anywhere in the group, alpha cleared
 * only in the sticks' rectangles, and left alone with touchpad controls. */
#include "game_patches.h"
#include "posix_compat.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t RECORD[] = {
    0x00, 0x20, 0x00, 0x10, 0x00, 0xbe, 0x32, 0x9b, 0x61, 0x0c, 0x43, 0x24, 0x00, 0x00,
    0x06, 0x00, 0x10, 0x00, 0x10, 0x05, 0x00, 0x10, 0x00, 0x04, 0x00, 0x02, 0x00, 0x08,
};

int main(void) {
    enum { WIDTH = 1024, HEIGHT = 512, PREFIX = 777 };
    size_t size = PREFIX + sizeof(RECORD) + WIDTH * HEIGHT * 2 + 16;
    uint8_t *data = malloc(size);
    memset(data, 0xff, size);
    memcpy(data + PREFIX, RECORD, sizeof(RECORD));
    uint8_t *pixels = data + PREFIX + sizeof(RECORD);

    assert(hud_sticks_apply_to("ingame/INGAME.group.bin"));
    assert(!hud_sticks_apply_to("ingame/weapons_kino.group.bin"));
    assert(hud_sticks_hide(data, size));
    assert(pixels[(10 * WIDTH + 10) * 2] == 0xf0);          /* inside the left stick */
    assert(pixels[(10 * WIDTH + 10) * 2 + 1] == 0xff);      /* colour kept */
    assert(pixels[(300 * WIDTH + 500) * 2] == 0xff);        /* outside every stick */
    assert(pixels[(450 * WIDTH + 650) * 2] == 0xf0);        /* the third rectangle */

    memset(data + PREFIX, 0, sizeof(RECORD));
    assert(!hud_sticks_hide(data, size));                   /* no HUD_ALL record: untouched */
    assert(!hud_sticks_hide(data, PREFIX + sizeof(RECORD))); /* record without its texels */

    setenv("BOZ_TOUCHPAD_CONTROLS", "1", 1);
    assert(!hud_sticks_apply_to("ingame.group.bin"));
    free(data);
    puts("hud_sticks_test: ok");
    return 0;
}
