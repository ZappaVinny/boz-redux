#ifndef BOZ_TOUCH_CONTROLS_H
#define BOZ_TOUCH_CONTROLS_H

/* On-screen controls for a Zombies match on a touch screen (phones and tablets), from BOZ
 * Reimagined. s3e_input.c feeds fingers in and takes the result into the same native controls
 * as the keyboard and mouse: the joystick is the movement keys, a dragging finger is mouse look,
 * each button is its action. The left part of the screen is a floating joystick, the rest looks
 * around, the buttons win over both; a finger on FIRE also looks, so the player can aim while
 * shooting. Positions are in the game's 1280x720 reference units. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum touch_phase { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

void touch_controls_finger(int64_t id, enum touch_phase phase, float x, float y);
void touch_controls_reset(void);
/* The joystick: x right, y back (forward is negative), length at most 1. */
void touch_controls_move(float *x, float *y);
/* Look movement in reference units since the last call. */
void touch_controls_take_look(float *dx, float *dy);
/* A finger is on the pause area (the game's own pause icon, top left). */
bool touch_controls_pause_held(void);
/* The joystick is pushed forward to its edge: sprint. */
bool touch_controls_sprint(void);
/* enum native_action */
bool touch_controls_down(int action);

/* What to draw, in reference units (overlay.cpp draws it). */
struct touch_shape {
    float x, y, r;
    uint32_t fill;    /* 0xAARRGGBB */
    uint32_t outline; /* 0 for none */
    const char *label;
};
int touch_controls_shapes(struct touch_shape *out, int capacity);

/* Shown while a Zombies match runs and the last input came from the touch screen. */
void touch_controls_set_visible(bool visible);
bool touch_controls_visible(void);

#ifdef __cplusplus
}
#endif

#endif
