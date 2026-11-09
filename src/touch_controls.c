/* On-screen controls for a Zombies match on a touch screen; see include/touch_controls.h. */
#include "touch_controls.h"

#include "native_input.h"

#include <math.h>
#include <string.h>

enum {
    MAX_FINGERS = 10,
    ROLE_NONE = 0,
    ROLE_STICK,
    ROLE_LOOK,
    ROLE_BUTTON,
    ROLE_PAUSE,
};

#define STICK_REST_X 210.0f
#define STICK_REST_Y 530.0f
#define STICK_RADIUS 105.0f
#define STICK_DEADZONE 0.12f
#define STICK_AREA_RIGHT 560.0f
#define PAUSE_RIGHT 100.0f
#define PAUSE_BOTTOM 90.0f
#define SPRINT_ON 0.92f
#define SPRINT_OFF 0.75f

struct button {
    int action;
    float x, y, r;
    const char *label;
};

static const struct button k_buttons[] = {
    {NATIVE_SHOOT, 1130, 565, 95, "FIRE"},
    {NATIVE_AIM, 965, 625, 58, "AIM"},
    {NATIVE_RELOAD, 985, 480, 54, "RELOAD"},
    {NATIVE_USE, 1125, 395, 58, "USE"},
    {NATIVE_GRENADE, 1235, 425, 46, "GREN"},
    {NATIVE_MELEE, 835, 650, 50, "KNIFE"},
    {NATIVE_SWITCH_WEAPON, 845, 525, 46, "SWAP"},
    {NATIVE_TACTICAL, 1000, 345, 44, "TACT"},
    {NATIVE_CROUCH, 90, 300, 50, "CROUCH"},
};
#define BUTTON_COUNT ((int)(sizeof(k_buttons) / sizeof(k_buttons[0])))

static struct finger {
    int64_t id;
    int role;
    int button;
    float x, y;
} g_fingers[MAX_FINGERS];

static float g_stick_x = STICK_REST_X, g_stick_y = STICK_REST_Y, g_knob_x, g_knob_y;
static bool g_stick_held;
static float g_look_dx, g_look_dy;
static bool g_sprint;
static bool g_visible;

static int button_at(float x, float y) {
    int best = -1;
    float best_distance = 0;
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        float dx = x - k_buttons[i].x, dy = y - k_buttons[i].y;
        float distance = sqrtf(dx * dx + dy * dy);
        /* a little more than drawn: thumbs land off-centre */
        if (distance <= k_buttons[i].r * 1.15f && (best < 0 || distance < best_distance)) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

static bool button_held(int index) {
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (g_fingers[i].role == ROLE_BUTTON && g_fingers[i].button == index) {
            return true;
        }
    }
    return false;
}

static struct finger *finger_find(int64_t id) {
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (g_fingers[i].role != ROLE_NONE && g_fingers[i].id == id) {
            return &g_fingers[i];
        }
    }
    return NULL;
}

static void finger_down(int64_t id, float x, float y) {
    struct finger *f = NULL;
    for (int i = 0; i < MAX_FINGERS && !f; ++i) {
        if (g_fingers[i].role == ROLE_NONE) {
            f = &g_fingers[i];
        }
    }
    if (!f) {
        return;
    }
    f->id = id;
    f->x = x;
    f->y = y;
    int button = button_at(x, y);
    if (x < PAUSE_RIGHT && y < PAUSE_BOTTOM) {
        f->role = ROLE_PAUSE;
    } else if (button >= 0) {
        f->role = ROLE_BUTTON;
        f->button = button;
    } else if (x < STICK_AREA_RIGHT && !g_stick_held) {
        f->role = ROLE_STICK;
        g_stick_held = true;
        g_stick_x = g_knob_x = x;
        g_stick_y = g_knob_y = y;
    } else {
        f->role = ROLE_LOOK;
    }
}

void touch_controls_finger(int64_t id, enum touch_phase phase, float x, float y) {
    if (phase == TOUCH_DOWN) {
        finger_down(id, x, y);
        return;
    }
    struct finger *f = finger_find(id);
    if (!f) {
        return; /* it landed before the match, or on another screen */
    }
    if (phase == TOUCH_UP) {
        if (f->role == ROLE_STICK) {
            g_stick_held = false;
            g_stick_x = STICK_REST_X;
            g_stick_y = STICK_REST_Y;
        }
        f->role = ROLE_NONE;
        return;
    }
    if (f->role == ROLE_STICK) {
        g_knob_x = x;
        g_knob_y = y;
    } else if (f->role == ROLE_LOOK ||
               (f->role == ROLE_BUTTON && k_buttons[f->button].action == NATIVE_SHOOT)) {
        g_look_dx += x - f->x;
        g_look_dy += y - f->y;
    }
    f->x = x;
    f->y = y;
}

void touch_controls_reset(void) {
    memset(g_fingers, 0, sizeof(g_fingers));
    g_stick_held = false;
    g_stick_x = STICK_REST_X;
    g_stick_y = STICK_REST_Y;
    g_look_dx = g_look_dy = 0;
    g_sprint = false;
}

void touch_controls_move(float *x, float *y) {
    *x = *y = 0;
    if (!g_stick_held) {
        return;
    }
    float dx = (g_knob_x - g_stick_x) / STICK_RADIUS, dy = (g_knob_y - g_stick_y) / STICK_RADIUS;
    float length = sqrtf(dx * dx + dy * dy);
    if (length < STICK_DEADZONE) {
        return;
    }
    if (length > 1.0f) {
        dx /= length;
        dy /= length;
    }
    *x = dx;
    *y = dy;
}

void touch_controls_take_look(float *dx, float *dy) {
    *dx = g_look_dx;
    *dy = g_look_dy;
    g_look_dx = g_look_dy = 0;
}

bool touch_controls_pause_held(void) {
    for (int i = 0; i < MAX_FINGERS; ++i) {
        if (g_fingers[i].role == ROLE_PAUSE) {
            return true;
        }
    }
    return false;
}

bool touch_controls_sprint(void) {
    float x, y;
    touch_controls_move(&x, &y);
    float length = sqrtf(x * x + y * y);
    /* forward only, and a band so the thumb does not flicker it at the edge */
    if (y > -0.5f || length < SPRINT_OFF) {
        g_sprint = false;
    } else if (length >= SPRINT_ON) {
        g_sprint = true;
    }
    return g_sprint;
}

bool touch_controls_down(int action) {
    for (int i = 0; i < BUTTON_COUNT; ++i) {
        if (k_buttons[i].action == action && button_held(i)) {
            return true;
        }
    }
    return false;
}

int touch_controls_shapes(struct touch_shape *out, int capacity) {
    int n = 0;
    float mx, my;
    touch_controls_move(&mx, &my);
    if (n < capacity) {
        out[n++] = (struct touch_shape){g_stick_x, g_stick_y, STICK_RADIUS, 0x38000000u,
                                        0x30ffffffu, NULL};
    }
    if (n < capacity) {
        out[n++] = (struct touch_shape){g_stick_x + mx * STICK_RADIUS, g_stick_y + my * STICK_RADIUS,
                                        44, g_sprint ? 0xc0d02020u : 0x90f5f0e0u, 0, NULL};
    }
    for (int i = 0; i < BUTTON_COUNT && n < capacity; ++i) {
        const struct button *b = &k_buttons[i];
        bool held = button_held(i);
        out[n++] = (struct touch_shape){b->x, b->y, b->r, held ? 0xb0d02020u : 0x50000000u,
                                        held ? 0xb0ff4040u : 0x60ffffffu, b->label};
    }
    /* the pause icon is the game's own: only its touch area is here */
    return n;
}

void touch_controls_set_visible(bool visible) {
    if (!visible && g_visible) {
        touch_controls_reset();
    }
    g_visible = visible;
}

bool touch_controls_visible(void) {
    return g_visible;
}
