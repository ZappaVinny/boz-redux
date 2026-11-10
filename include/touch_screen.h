#ifndef BOZ_TOUCH_SCREEN_H
#define BOZ_TOUCH_SCREEN_H

#include <stdbool.h>
#include <stdlib.h>

/* Touch-screen play (phones): the game runs as on an ordinary Android phone, not the Xperia Play
 * the PC client presents. Its own touch controls (Options > Controls), all fingers passed through
 * as touches, and none of the PC input patches (mouse and keyboard player input, the hidden
 * on-screen sticks and prompt button). Always on for Android; BOZ_TOUCH_SCREEN=1 turns it on
 * elsewhere to test it. */
static inline bool touch_screen_mode(void) {
#if defined(__ANDROID__)
    return true;
#else
    static int mode = -1;
    if (mode < 0) {
        const char *value = getenv("BOZ_TOUCH_SCREEN");
        mode = value && value[0] == '1';
    }
    return mode != 0;
#endif
}

#endif
