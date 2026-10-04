#include "s3e_host_internal.h"

#include "client_config.h"
#include "mod_runtime.h"
#include "native_input.h"

#include <strings.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

enum {
    SDL_INIT_JOYSTICK = 0x00000200u,
    SDL_INIT_GAMECONTROLLER = 0x00002000u,
    SDL_FIRSTEVENT = 0,
    SDL_LASTEVENT = 0xffff,
};

enum {
    SDL_BUTTON_A = 0,
    SDL_BUTTON_B = 1,
    SDL_BUTTON_X = 2,
    SDL_BUTTON_Y = 3,
    SDL_BUTTON_BACK = 4,
    SDL_BUTTON_START = 6,
    SDL_BUTTON_LEFTSHOULDER = 9,
    SDL_BUTTON_RIGHTSHOULDER = 10,
    SDL_BUTTON_DPAD_UP = 11,
    SDL_BUTTON_DPAD_DOWN = 12,
    SDL_BUTTON_DPAD_LEFT = 13,
    SDL_BUTTON_DPAD_RIGHT = 14,
};

enum {
    SDL_AXIS_LEFTX = 0,
    SDL_AXIS_LEFTY = 1,
    SDL_AXIS_RIGHTX = 2,
    SDL_AXIS_RIGHTY = 3,
    SDL_AXIS_TRIGGERLEFT = 4,
    SDL_AXIS_TRIGGERRIGHT = 5,
};

enum {
    SDL_HAT_UP = 0x01,
    SDL_HAT_RIGHT = 0x02,
    SDL_HAT_DOWN = 0x04,
    SDL_HAT_LEFT = 0x08,
};

enum {
    POINTER_STATE_UP = 0,
    POINTER_STATE_DOWN = 1,
    POINTER_STATE_PRESSED = 2,
    POINTER_STATE_RELEASED = 4,
};

enum {
    KEY_STATE_DOWN = 1,
    KEY_STATE_PRESSED = 2,
    KEY_STATE_RELEASED = 4,
};

enum {
    BINDING_KEY_SHOOT = 8,
    BINDING_KEY_CHANGE_WEAPON = 14,
    BINDING_KEY_CROUCH_PRONE = 48,
    BINDING_KEY_RELOAD = 40,
    BINDING_KEY_MELEE = 44,
    BINDING_KEY_ALTERNATE_FIRE = 18,
    BINDING_KEY_AIM = 100,
    BINDING_KEY_THROW_GRENADE = 29,
    BINDING_KEY_TACTICAL_GRENADE = 42,
    BINDING_KEY_ACTION_SPRINT = 28,
    BINDING_KEY_TOGGLE_FREE_MODE = 39,
    XPERIA_KEY_ALTERNATE_FIRE = 9,
    XPERIA_KEY_TACTICAL_GRENADE = 10,
    XPERIA_KEY_CHANGE_WEAPON = 11,
    XPERIA_KEY_CROUCH_PRONE = 12,
    XPERIA_KEY_AIM = 74,
    XPERIA_KEY_SHOOT = 75,
    XPERIA_KEY_ACTION_SPRINT = 78,
    XPERIA_KEY_MELEE = 89,
    XPERIA_KEY_THROW_GRENADE = 90,
    XPERIA_KEY_RELOAD = 126,
    XPERIA_KEY_PAUSE = 72,
    S3E_KEY_ABS_GAME_A = 200,
    S3E_KEY_ABS_GAME_B = 201,
    S3E_KEY_ABS_GAME_C = 202,
    S3E_KEY_ABS_GAME_D = 203,
    S3E_KEY_ABS_UP = 204,
    S3E_KEY_ABS_DOWN = 205,
    S3E_KEY_ABS_LEFT = 206,
    S3E_KEY_ABS_RIGHT = 207,
    S3E_KEY_ABS_OK = 208,
    S3E_KEY_ABS_ASK = 209,
    S3E_KEY_ABS_BSK = 210,
};

enum {
    KEYBOARD_KEY_COUNT = 256,
    TOUCHPAD_COUNT = 2,
    CONTROLLER_RESCAN_MS = 1000,
    AXIS_DEADZONE = 9000,
    XPERIA_AXIS_DEADZONE = 6000,
    TRIGGER_PRESS_THRESHOLD = 16384,
    TRIGGER_RELEASE_THRESHOLD = 12288,
};

enum trigger_id {
    TRIGGER_LEFT,
    TRIGGER_RIGHT,
    TRIGGER_COUNT,
};

enum {
    S3E_TOUCHPAD_RELEASED = 0,
    S3E_TOUCHPAD_PRESSED = 1,
};

struct key_map {
    const uint32_t *keys;
    size_t key_count;
};

struct sdl_input_api {
    int (*InitSubSystem)(uint32_t flags);
    void (*QuitSubSystem)(uint32_t flags);
    void (*PumpEvents)(void);
    void (*FlushEvents)(uint32_t min_type, uint32_t max_type);
    int (*NumJoysticks)(void);
    int (*IsGameController)(int joystick_index);
    const char *(*JoystickNameForIndex)(int joystick_index);
    const char *(*GameControllerNameForIndex)(int joystick_index);
    void *(*GameControllerOpen)(int joystick_index);
    void (*GameControllerClose)(void *gamecontroller);
    int (*GameControllerGetAttached)(void *gamecontroller);
    void *(*GameControllerGetJoystick)(void *gamecontroller);
    int (*JoystickNumHats)(void *joystick);
    uint8_t (*JoystickGetHat)(void *joystick, int hat);
    void (*GameControllerUpdate)(void);
    int16_t (*GameControllerGetAxis)(void *gamecontroller, int axis);
    uint8_t (*GameControllerGetButton)(void *gamecontroller, int button);
    const char *(*GetError)(void);
    uint32_t (*GetMouseState)(int *x, int *y);
    uint32_t (*GetRelativeMouseState)(int *x, int *y);
    const uint8_t *(*GetKeyboardState)(int *count);
    int (*SetRelativeMouseMode)(int enabled);
    void *(*GetMouseFocus)(void);
    void *(*GetKeyboardFocus)(void);
    int (*GetScancodeFromName)(const char *name);
    uint32_t (*GetWindowFlags)(void *window);
    int (*SetWindowFullscreen)(void *window, uint32_t flags);
    void (*GetWindowSize)(void *window, int *w, int *h);
    int (*HasEvent)(uint32_t type);
    int (*PollEvent)(void *event);
    const char *(*GetScancodeName)(int scancode);
};

static const uint32_t KEY_ACTION[] = {XPERIA_KEY_ACTION_SPRINT};
static const uint32_t KEY_RELOAD[] = {XPERIA_KEY_RELOAD};
static const uint32_t KEY_MELEE[] = {XPERIA_KEY_MELEE};
static const uint32_t KEY_GRENADE[] = {XPERIA_KEY_THROW_GRENADE};
static const uint32_t KEY_AIM[] = {XPERIA_KEY_AIM};
static const uint32_t KEY_SHOOT[] = {XPERIA_KEY_SHOOT};
static const uint32_t KEY_TACTICAL[] = {XPERIA_KEY_TACTICAL_GRENADE};
static const uint32_t KEY_CROUCH[] = {XPERIA_KEY_CROUCH_PRONE};
static const uint32_t KEY_ALT_FIRE[] = {XPERIA_KEY_ALTERNATE_FIRE};
static const uint32_t KEY_CHANGE_WEAPON[] = {XPERIA_KEY_CHANGE_WEAPON};
static const uint32_t KEY_START[] = {XPERIA_KEY_PAUSE};

#define KEY_MAP(keys) {keys, ARRAY_SIZE(keys)}

static const struct key_map KEYMAP_ACTION = KEY_MAP(KEY_ACTION);
static const struct key_map KEYMAP_RELOAD = KEY_MAP(KEY_RELOAD);
static const struct key_map KEYMAP_MELEE = KEY_MAP(KEY_MELEE);
static const struct key_map KEYMAP_GRENADE = KEY_MAP(KEY_GRENADE);
static const struct key_map KEYMAP_AIM = KEY_MAP(KEY_AIM);
static const struct key_map KEYMAP_SHOOT = KEY_MAP(KEY_SHOOT);
static const struct key_map KEYMAP_TACTICAL = KEY_MAP(KEY_TACTICAL);
static const struct key_map KEYMAP_CROUCH = KEY_MAP(KEY_CROUCH);
static const struct key_map KEYMAP_ALT_FIRE = KEY_MAP(KEY_ALT_FIRE);
static const struct key_map KEYMAP_CHANGE_WEAPON = KEY_MAP(KEY_CHANGE_WEAPON);
static const struct key_map KEYMAP_START = KEY_MAP(KEY_START);

#undef KEY_MAP

static void *g_sdl2;
static struct sdl_input_api g_sdl;
static void *g_controller;
static void *g_joystick;
static int g_sdl_tried;
static int g_sdl_initialized;
static int g_input_pumping;
static int g_prev_select;
static int g_prev_a;
static uint64_t g_input_last_ms;
static uint64_t g_controller_scan_ms;
static int g_last_joystick_count = -1;
static uint8_t g_hat_mask;
static uint8_t g_trigger_down[TRIGGER_COUNT];

static uint8_t g_keyboard_live[KEYBOARD_KEY_COUNT];
static uint8_t g_keyboard_published[KEYBOARD_KEY_COUNT];

static int g_touchpad_active[TOUCHPAD_COUNT];
static int32_t g_touchpad_x[TOUCHPAD_COUNT];
static int32_t g_touchpad_y[TOUCHPAD_COUNT];
static uint8_t g_touchpad_state[TOUCHPAD_COUNT];

static int sdl_load_symbol(void **slot, const char *name) {
    *slot = plat_lib_symbol(g_sdl2, name);
    return *slot != NULL;
}

static void sdl_load_optional_symbol(void **slot, const char *name) {
    *slot = plat_lib_symbol(g_sdl2, name);
}

static const char *input_joystick_name(int index) {
    const char *name = NULL;
    if (g_sdl.GameControllerNameForIndex) {
        name = g_sdl.GameControllerNameForIndex(index);
    }
    if (!name && g_sdl.JoystickNameForIndex) {
        name = g_sdl.JoystickNameForIndex(index);
    }
    return name ? name : "unknown";
}

static void input_scan_controllers(void) {
    int count = g_sdl.NumJoysticks();
    int report = count != g_last_joystick_count;
    g_last_joystick_count = count;

    for (int i = 0; i < count; ++i) {
        if (!g_sdl.IsGameController(i)) {
            if (report) {
                fprintf(stderr, "[input] joystick %d (%s) has no SDL controller mapping\n", i,
                        input_joystick_name(i));
            }
            continue;
        }

        g_controller = g_sdl.GameControllerOpen(i);
        if (!g_controller) {
            if (report) {
                const char *error = g_sdl.GetError ? g_sdl.GetError() : NULL;
                fprintf(stderr, "[input] could not open controller %d (%s): %s\n", i,
                        input_joystick_name(i), error ? error : "unknown");
            }
            continue;
        }

        g_joystick =
            g_sdl.GameControllerGetJoystick ? g_sdl.GameControllerGetJoystick(g_controller) : NULL;
        fprintf(stderr, "[input] using controller %d (%s)\n", i, input_joystick_name(i));
        return;
    }
}

static void input_open(void) {
    if (g_sdl_tried) {
        return;
    }
    g_sdl_tried = 1;

    const char *names[] = {BOZ_LIB_SDL2, NULL};
    g_sdl2 = open_first(names);
    if (!g_sdl2) {
        return;
    }

    int ok = 1;
    ok &= sdl_load_symbol((void **)&g_sdl.InitSubSystem, "SDL_InitSubSystem");
    ok &= sdl_load_symbol((void **)&g_sdl.QuitSubSystem, "SDL_QuitSubSystem");
    ok &= sdl_load_symbol((void **)&g_sdl.PumpEvents, "SDL_PumpEvents");
    ok &= sdl_load_symbol((void **)&g_sdl.FlushEvents, "SDL_FlushEvents");
    ok &= sdl_load_symbol((void **)&g_sdl.NumJoysticks, "SDL_NumJoysticks");
    ok &= sdl_load_symbol((void **)&g_sdl.IsGameController, "SDL_IsGameController");
    sdl_load_optional_symbol((void **)&g_sdl.JoystickNameForIndex, "SDL_JoystickNameForIndex");
    sdl_load_optional_symbol((void **)&g_sdl.GameControllerNameForIndex,
                             "SDL_GameControllerNameForIndex");
    ok &= sdl_load_symbol((void **)&g_sdl.GameControllerOpen, "SDL_GameControllerOpen");
    ok &= sdl_load_symbol((void **)&g_sdl.GameControllerClose, "SDL_GameControllerClose");
    ok &=
        sdl_load_symbol((void **)&g_sdl.GameControllerGetAttached, "SDL_GameControllerGetAttached");
    sdl_load_optional_symbol((void **)&g_sdl.GameControllerGetJoystick,
                             "SDL_GameControllerGetJoystick");
    sdl_load_optional_symbol((void **)&g_sdl.JoystickNumHats, "SDL_JoystickNumHats");
    sdl_load_optional_symbol((void **)&g_sdl.JoystickGetHat, "SDL_JoystickGetHat");
    ok &= sdl_load_symbol((void **)&g_sdl.GameControllerUpdate, "SDL_GameControllerUpdate");
    ok &= sdl_load_symbol((void **)&g_sdl.GameControllerGetAxis, "SDL_GameControllerGetAxis");
    ok &= sdl_load_symbol((void **)&g_sdl.GameControllerGetButton, "SDL_GameControllerGetButton");
    sdl_load_optional_symbol((void **)&g_sdl.GetError, "SDL_GetError");
    sdl_load_optional_symbol((void **)&g_sdl.GetMouseState, "SDL_GetMouseState");
    sdl_load_optional_symbol((void **)&g_sdl.GetRelativeMouseState, "SDL_GetRelativeMouseState");
    sdl_load_optional_symbol((void **)&g_sdl.GetKeyboardState, "SDL_GetKeyboardState");
    sdl_load_optional_symbol((void **)&g_sdl.SetRelativeMouseMode, "SDL_SetRelativeMouseMode");
    sdl_load_optional_symbol((void **)&g_sdl.GetMouseFocus, "SDL_GetMouseFocus");
    sdl_load_optional_symbol((void **)&g_sdl.GetKeyboardFocus, "SDL_GetKeyboardFocus");
    sdl_load_optional_symbol((void **)&g_sdl.GetScancodeFromName, "SDL_GetScancodeFromName");
    sdl_load_optional_symbol((void **)&g_sdl.GetWindowFlags, "SDL_GetWindowFlags");
    sdl_load_optional_symbol((void **)&g_sdl.SetWindowFullscreen, "SDL_SetWindowFullscreen");
    sdl_load_optional_symbol((void **)&g_sdl.GetWindowSize, "SDL_GetWindowSize");
    sdl_load_optional_symbol((void **)&g_sdl.HasEvent, "SDL_HasEvent");
    sdl_load_optional_symbol((void **)&g_sdl.PollEvent, "SDL_PollEvent");
    sdl_load_optional_symbol((void **)&g_sdl.GetScancodeName, "SDL_GetScancodeName");
    if (!ok || g_sdl.InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        if (!ok) {
            fprintf(stderr, "[input] SDL2 controller symbols unavailable\n");
        } else if (g_sdl.GetError) {
            const char *error = g_sdl.GetError();
            fprintf(stderr, "[input] SDL_InitSubSystem failed: %s\n", error ? error : "unknown");
        }
        return;
    }
    g_sdl_initialized = 1;
}

static uint8_t input_current_hat_mask(void) {
    uint8_t mask = 0;
    if (!g_joystick || !g_sdl.JoystickNumHats || !g_sdl.JoystickGetHat) {
        return 0;
    }

    int count = g_sdl.JoystickNumHats(g_joystick);
    for (int i = 0; i < count; ++i) {
        mask |= g_sdl.JoystickGetHat(g_joystick, i);
    }
    return mask;
}

static int input_button(int button) {
    if (!g_controller || !g_sdl.GameControllerGetButton || button < 0) {
        return 0;
    }
    return g_sdl.GameControllerGetButton(g_controller, button) != 0;
}

static int32_t input_axis_raw(int axis) {
    if (!g_controller || !g_sdl.GameControllerGetAxis) {
        return 0;
    }
    return g_sdl.GameControllerGetAxis(g_controller, axis);
}

static int input_trigger(int axis, enum trigger_id trigger) {
    int32_t value = input_axis_raw(axis);
    if (g_trigger_down[trigger]) {
        if (value < TRIGGER_RELEASE_THRESHOLD) {
            g_trigger_down[trigger] = 0;
        }
    } else if (value > TRIGGER_PRESS_THRESHOLD) {
        g_trigger_down[trigger] = 1;
    }
    return g_trigger_down[trigger];
}

static int input_hat(uint8_t mask) {
    return (g_hat_mask & mask) != 0;
}

static int input_dpad_up(void) {
    return input_button(SDL_BUTTON_DPAD_UP) || input_hat(SDL_HAT_UP);
}

static int input_dpad_down(void) {
    return input_button(SDL_BUTTON_DPAD_DOWN) || input_hat(SDL_HAT_DOWN);
}

static int input_dpad_left(void) {
    return input_button(SDL_BUTTON_DPAD_LEFT) || input_hat(SDL_HAT_LEFT);
}

static int input_dpad_right(void) {
    return input_button(SDL_BUTTON_DPAD_RIGHT) || input_hat(SDL_HAT_RIGHT);
}

static int32_t input_axis_deadzone(int axis, int32_t deadzone) {
    int32_t value = input_axis_raw(axis);
    return value < -deadzone || value > deadzone ? value : 0;
}

static int32_t input_axis(int axis) {
    return input_axis_deadzone(axis, AXIS_DEADZONE);
}

static int32_t input_xperia_axis(int axis) {
    return input_axis_deadzone(axis, XPERIA_AXIS_DEADZONE);
}

static int32_t window_width(void) {
    return g_surface.width > 0 ? (int32_t)g_surface.width : 640;
}

static int32_t window_height(void) {
    return g_surface.height > 0 ? (int32_t)g_surface.height : 480;
}

static int32_t clamp_value(int32_t value, int32_t upper_exclusive) {
    if (value < 0) {
        return 0;
    }
    if (value >= upper_exclusive) {
        return upper_exclusive - 1;
    }
    return value;
}

static int32_t clamp_pointer_x(int32_t x) {
    return clamp_value(x, window_width());
}

static int32_t clamp_pointer_y(int32_t y) {
    return clamp_value(y, window_height());
}

static void pointer_dispatch(uint32_t id, void *event) {
    if (id >= ARRAY_SIZE(g_pointer_callbacks)) {
        return;
    }
    struct callback_slot *slot = &g_pointer_callbacks[id];
    if (!slot->callback) {
        return;
    }
    s3e_guest_call(slot->callback, 2, S3E_GUEST_ARG(event), S3E_GUEST_ARG(slot->user_data));
}

static void touchpad_dispatch(uint32_t id, void *event) {
    if (id >= ARRAY_SIZE(g_touchpad_callbacks)) {
        return;
    }
    struct callback_slot *slot = &g_touchpad_callbacks[id];
    if (!slot->callback) {
        return;
    }
    s3e_guest_call(slot->callback, 2, S3E_GUEST_ARG(event), S3E_GUEST_ARG(slot->user_data));
}

static void pointer_set_down(int down) {
    if (down) {
        g_pointer_states[0] = g_pointer_down ? POINTER_STATE_DOWN : POINTER_STATE_PRESSED;
        g_pointer_down = 1;
    } else {
        g_pointer_states[0] = g_pointer_down ? POINTER_STATE_RELEASED : POINTER_STATE_UP;
        g_pointer_down = 0;
    }
}

static void pointer_clear_transitions(void) {
    for (size_t i = 0; i < sizeof(g_pointer_states); ++i) {
        if (g_pointer_states[i] == POINTER_STATE_PRESSED) {
            g_pointer_states[i] = POINTER_STATE_DOWN;
        } else if (g_pointer_states[i] == POINTER_STATE_RELEASED) {
            g_pointer_states[i] = POINTER_STATE_UP;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(g_touchpad_state); ++i) {
        if (g_touchpad_state[i] == POINTER_STATE_PRESSED) {
            g_touchpad_state[i] = POINTER_STATE_DOWN;
        } else if (g_touchpad_state[i] == POINTER_STATE_RELEASED) {
            g_touchpad_state[i] = POINTER_STATE_UP;
        }
    }
}

static void pointer_dispatch_button(uint32_t button, int32_t pressed) {
    struct s3e_pointer_button_event event = {
        .button = (int32_t)button,
        .pressed = pressed,
        .x = g_pointer_x,
        .y = g_pointer_y,
    };
    struct s3e_pointer_touch_event touch_event = {
        .touch_id = (int32_t)button,
        .pressed = pressed,
        .x = g_pointer_x,
        .y = g_pointer_y,
    };
    pointer_dispatch(0, &event);
    pointer_dispatch(2, &touch_event);
}

static void pointer_dispatch_motion(void) {
    struct s3e_pointer_motion_event event = {
        .x = g_pointer_x,
        .y = g_pointer_y,
    };
    struct s3e_pointer_touch_motion_event touch_event = {
        .touch_id = 0,
        .x = g_pointer_x,
        .y = g_pointer_y,
    };
    pointer_dispatch(1, &event);
    pointer_dispatch(3, &touch_event);
}

static void input_release_pointer(void) {
    if (g_pointer_down) {
        pointer_set_down(0);
        pointer_dispatch_button(0, 0);
    }
}

static void keyboard_dispatch_event(uint32_t key, int32_t pressed) {
    struct s3e_keyboard_event event = {
        .key = (int32_t)key,
        .pressed = pressed ? 1 : 0,
    };

    for (size_t i = 0; i < ARRAY_SIZE(g_keyboard_callbacks); ++i) {
        struct keyboard_callback_slot *slot = &g_keyboard_callbacks[i];
        if (slot->callback && slot->id == 0) {
            s3e_guest_call(slot->callback, 2, S3E_GUEST_ARG(&event), S3E_GUEST_ARG(slot->user_data));
        }
    }
}

static void keyboard_set_key(uint32_t key, int down, int dispatch_callback) {
    if (key >= KEYBOARD_KEY_COUNT) {
        return;
    }

    int was_down = (g_keyboard_live[key] & KEY_STATE_DOWN) != 0;
    if (was_down == down) {
        return;
    }

    if (down) {
        g_keyboard_live[key] |= KEY_STATE_DOWN | KEY_STATE_PRESSED;
    } else {
        g_keyboard_live[key] &= (uint8_t)~KEY_STATE_DOWN;
        g_keyboard_live[key] |= KEY_STATE_RELEASED;
    }
    if (dispatch_callback) {
        keyboard_dispatch_event(key, down);
    }
}

static void keyboard_set_keys(const uint32_t *keys, size_t count, int down, int dispatch_callback) {
    for (size_t i = 0; i < count; ++i) {
        keyboard_set_key(keys[i], down, dispatch_callback);
    }
}

static void keyboard_publish(void) {
    memcpy(g_keyboard_published, g_keyboard_live, sizeof(g_keyboard_published));
    for (size_t key = 0; key < ARRAY_SIZE(g_keyboard_live); ++key) {
        g_keyboard_live[key] &= (uint8_t)~(KEY_STATE_PRESSED | KEY_STATE_RELEASED);
    }
}

static uint32_t keyboard_abs_target(uint32_t key) {
    switch (key) {
    case S3E_KEY_ABS_GAME_A:
        return BINDING_KEY_ACTION_SPRINT;
    case S3E_KEY_ABS_GAME_B:
        return BINDING_KEY_RELOAD;
    case S3E_KEY_ABS_GAME_C:
        return BINDING_KEY_THROW_GRENADE;
    case S3E_KEY_ABS_GAME_D:
        return BINDING_KEY_MELEE;
    case S3E_KEY_ABS_UP:
        return BINDING_KEY_TACTICAL_GRENADE;
    case S3E_KEY_ABS_DOWN:
        return BINDING_KEY_CROUCH_PRONE;
    case S3E_KEY_ABS_LEFT:
        return BINDING_KEY_ALTERNATE_FIRE;
    case S3E_KEY_ABS_RIGHT:
        return BINDING_KEY_CHANGE_WEAPON;
    case S3E_KEY_ABS_OK:
        return BINDING_KEY_ACTION_SPRINT;
    case S3E_KEY_ABS_ASK:
        return XPERIA_KEY_PAUSE;
    case S3E_KEY_ABS_BSK:
        return BINDING_KEY_RELOAD;
    default:
        return key;
    }
}

static void game_action_apply(int down, const struct key_map *keys) {
    keyboard_set_keys(keys->keys, keys->key_count, down, 1);
}

static void keyboard_release_all(void) {
    for (uint32_t key = 0; key < KEYBOARD_KEY_COUNT; ++key) {
        if (g_keyboard_live[key] & KEY_STATE_DOWN) {
            keyboard_set_key(key, 0, 1);
        }
    }
}

static void input_reset_triggers(void) {
    memset(g_trigger_down, 0, sizeof(g_trigger_down));
}

static void touchpad_dispatch_button(uint32_t id, int32_t pressed, int32_t x, int32_t y) {
    struct s3e_touchpad_button_event event = {
        .id = (int32_t)id,
        .pressed = pressed,
        .x = x,
        .y = y,
    };
    touchpad_dispatch(0, &event);
}

static void touchpad_dispatch_motion(uint32_t id, int32_t x, int32_t y) {
    struct s3e_touchpad_motion_event event = {
        .id = (int32_t)id,
        .x = x,
        .y = y,
    };
    touchpad_dispatch(1, &event);
}

static void touchpad_release(uint32_t id) {
    if (id >= TOUCHPAD_COUNT || !g_touchpad_active[id]) {
        return;
    }
    g_touchpad_active[id] = 0;
    g_touchpad_state[id] = POINTER_STATE_RELEASED;
    touchpad_dispatch_button(id, S3E_TOUCHPAD_RELEASED, g_touchpad_x[id], g_touchpad_y[id]);
}

static void touchpad_release_all(void) {
    for (uint32_t id = 0; id < TOUCHPAD_COUNT; ++id) {
        touchpad_release(id);
    }
}

static int32_t touchpad_x_from_axis(int32_t center, int32_t radius, int32_t axis) {
    return clamp_value(center + (int32_t)((int64_t)axis * radius / 32767), XPERIA_TOUCHPAD_WIDTH);
}

static int32_t touchpad_y_from_axis(int32_t center, int32_t radius, int32_t axis) {
    return clamp_value(center + (int32_t)((int64_t)axis * radius / 32767), XPERIA_TOUCHPAD_HEIGHT);
}

static void touchpad_update_stick(uint32_t id, int32_t x_axis, int32_t y_axis, int32_t center_x,
                                  int32_t center_y, int32_t radius_x, int32_t radius_y) {
    if (id >= TOUCHPAD_COUNT) {
        return;
    }

    if (!x_axis && !y_axis) {
        touchpad_release(id);
        return;
    }

    int32_t x = touchpad_x_from_axis(center_x, radius_x, x_axis);
    int32_t y = touchpad_y_from_axis(center_y, radius_y, y_axis);

    if (!g_touchpad_active[id]) {
        g_touchpad_active[id] = 1;
        g_touchpad_x[id] = x;
        g_touchpad_y[id] = y;
        g_touchpad_state[id] = POINTER_STATE_PRESSED;
        touchpad_dispatch_motion(id, x, y);
        touchpad_dispatch_button(id, S3E_TOUCHPAD_PRESSED, x, y);
        return;
    }

    if (g_touchpad_x[id] != x || g_touchpad_y[id] != y) {
        g_touchpad_x[id] = x;
        g_touchpad_y[id] = y;
        touchpad_dispatch_motion(id, g_touchpad_x[id], g_touchpad_y[id]);
    }
}

static void input_update_cursor(uint64_t dt) {
    int32_t x_axis = input_axis(SDL_AXIS_LEFTX);
    int32_t y_axis = input_axis(SDL_AXIS_LEFTY);
    if (input_dpad_left()) {
        x_axis = -32767;
    } else if (input_dpad_right()) {
        x_axis = 32767;
    }
    if (input_dpad_up()) {
        y_axis = -32767;
    } else if (input_dpad_down()) {
        y_axis = 32767;
    }
    if (x_axis || y_axis) {
        int32_t old_x = g_pointer_x;
        int32_t old_y = g_pointer_y;
        const int32_t x_speed = (int32_t)((int64_t)900 * window_width() / 640);
        const int32_t y_speed = (int32_t)((int64_t)900 * window_height() / 480);
        g_pointer_x = clamp_pointer_x(
            g_pointer_x + (int32_t)((int64_t)x_axis * (int64_t)dt * x_speed / 32767 / 1000));
        g_pointer_y = clamp_pointer_y(
            g_pointer_y + (int32_t)((int64_t)y_axis * (int64_t)dt * y_speed / 32767 / 1000));
        if (g_pointer_x != old_x || g_pointer_y != old_y) {
            pointer_dispatch_motion();
        }
    }

    int a = input_button(SDL_BUTTON_A);
    if (a != g_prev_a) {
        pointer_set_down(a);
        pointer_dispatch_button(0, a ? 1 : 0);
    }
    g_prev_a = a;
}

static void input_update_game_keys(void) {
    g_prev_a = input_button(SDL_BUTTON_A);

    game_action_apply(input_button(SDL_BUTTON_A), &KEYMAP_ACTION);
    game_action_apply(input_button(SDL_BUTTON_B), &KEYMAP_RELOAD);
    game_action_apply(input_button(SDL_BUTTON_X), &KEYMAP_MELEE);
    game_action_apply(input_button(SDL_BUTTON_Y), &KEYMAP_GRENADE);
    game_action_apply(input_button(SDL_BUTTON_LEFTSHOULDER) ||
                          input_trigger(SDL_AXIS_TRIGGERLEFT, TRIGGER_LEFT),
                      &KEYMAP_AIM);
    game_action_apply(input_button(SDL_BUTTON_RIGHTSHOULDER) ||
                          input_trigger(SDL_AXIS_TRIGGERRIGHT, TRIGGER_RIGHT),
                      &KEYMAP_SHOOT);
    game_action_apply(input_button(SDL_BUTTON_START), &KEYMAP_START);
    game_action_apply(input_dpad_up(), &KEYMAP_TACTICAL);
    game_action_apply(input_dpad_down(), &KEYMAP_CROUCH);
    game_action_apply(input_dpad_left(), &KEYMAP_ALT_FIRE);
    game_action_apply(input_dpad_right(), &KEYMAP_CHANGE_WEAPON);
}

static void input_update_game_touchpads(void) {
    int32_t width = XPERIA_TOUCHPAD_WIDTH;
    int32_t height = XPERIA_TOUCHPAD_HEIGHT;
    int32_t center_y = height / 2;
    int32_t look_radius = width / 8;
    touchpad_update_stick(0, input_xperia_axis(SDL_AXIS_LEFTX), input_xperia_axis(SDL_AXIS_LEFTY),
                          width / 5, center_y, width / 5, height / 2);
    touchpad_update_stick(1, input_xperia_axis(SDL_AXIS_RIGHTX), input_xperia_axis(SDL_AXIS_RIGHTY),
                          (width * 4) / 5, center_y, look_radius, look_radius);
}

enum {
    SDL_QUIT_EVENT = 0x100,
    SC_A = 4,
    SC_C = 6,
    SC_D = 7,
    SC_E = 8,
    SC_F = 9,
    SC_G = 10,
    SC_Q = 20,
    SC_R = 21,
    SC_S = 22,
    SC_V = 25,
    SC_W = 26,
    SC_X = 27,
    SC_1 = 30,
    SC_ESCAPE = 41,
    SC_TAB = 43,
    SC_RETURN = 40,
    SC_F11 = 68,
    SC_LALT = 226,
    SC_RALT = 230,
    SC_SPACE = 44,
    SC_LSHIFT = 225,
    MOUSE_LEFT = 1u << 0,
    MOUSE_RIGHT = 1u << 2,
};

/* Desktop bindings: [keys] in client.ini, each a comma-separated list of SDL key names or
 * Mouse1..Mouse5. */
enum binding_id {
    BIND_MOVE_FORWARD,
    BIND_MOVE_BACK,
    BIND_MOVE_LEFT,
    BIND_MOVE_RIGHT,
    BIND_SHOOT,
    BIND_AIM,
    BIND_RELOAD,
    BIND_ACTION,
    BIND_SPRINT,
    BIND_MELEE,
    BIND_GRENADE,
    BIND_TACTICAL,
    BIND_CROUCH,
    BIND_ALT_FIRE,
    BIND_SWITCH_WEAPON,
    BIND_PAUSE,
    BIND_TOGGLE_MODE,
    BIND_FULLSCREEN,
    BIND_COUNT,
    BIND_MAX_KEYS = 6,
};

static const struct {
    const char *action;
    const char *defaults;
} BINDING_DEFAULTS[BIND_COUNT] = {
    [BIND_MOVE_FORWARD] = {"move_forward", "W"},
    [BIND_MOVE_BACK] = {"move_back", "S"},
    [BIND_MOVE_LEFT] = {"move_left", "A"},
    [BIND_MOVE_RIGHT] = {"move_right", "D"},
    [BIND_SHOOT] = {"shoot", "Mouse1"},
    [BIND_AIM] = {"aim", "Mouse3"},
    [BIND_RELOAD] = {"reload", "R"},
    [BIND_ACTION] = {"action", "E, F"},
    [BIND_SPRINT] = {"sprint", "Left Shift"},
    [BIND_MELEE] = {"melee", "V"},
    [BIND_GRENADE] = {"grenade", "G"},
    [BIND_TACTICAL] = {"tactical", "Q"},
    [BIND_CROUCH] = {"crouch", "C, Space"},
    [BIND_ALT_FIRE] = {"alt_fire", "X"},
    [BIND_SWITCH_WEAPON] = {"switch_weapon", "1"},
    [BIND_PAUSE] = {"pause", "Escape"},
    [BIND_TOGGLE_MODE] = {"toggle_mode", "Tab"},
    [BIND_FULLSCREEN] = {"fullscreen", "F11"},
};

static struct {
    int scancodes[BIND_MAX_KEYS];
    int scancode_count;
    uint32_t mouse_mask;
} g_bindings[BIND_COUNT];
static int g_bindings_loaded;
static uint32_t g_mouse_buttons;

static void parse_binding(enum binding_id id, const char *text) {
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", text);
    for (char *item = strtok(copy, ","); item; item = strtok(NULL, ",")) {
        while (*item == ' ' || *item == '\t') {
            ++item;
        }
        char *end = item + strlen(item);
        while (end > item && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        if (!*item) {
            continue;
        }
        if ((item[0] == 'M' || item[0] == 'm') && !strncasecmp(item, "mouse", 5) && item[5] >= '1' &&
            item[5] <= '5' && !item[6]) {
            g_bindings[id].mouse_mask |= 1u << (item[5] - '1');
            continue;
        }
        int scancode = g_sdl.GetScancodeFromName ? g_sdl.GetScancodeFromName(item) : 0;
        if (scancode <= 0) {
            fprintf(stderr, "[input] unknown key \"%s\" for %s\n", item,
                    BINDING_DEFAULTS[id].action);
        } else if (g_bindings[id].scancode_count < BIND_MAX_KEYS) {
            g_bindings[id].scancodes[g_bindings[id].scancode_count++] = scancode;
        }
    }
}

static void load_bindings(void) {
    if (g_bindings_loaded) {
        return;
    }
    g_bindings_loaded = 1;
    for (int id = 0; id < BIND_COUNT; ++id) {
        const char *configured = client_config_key_binding(BINDING_DEFAULTS[id].action);
        if (id == BIND_ACTION && configured && !strcmp(configured, "E, F, Left Shift")) {
            configured = NULL; /* the old default, from before sprint had its own key */
        }
        parse_binding((enum binding_id)id, configured ? configured : BINDING_DEFAULTS[id].defaults);
    }
}

static int binding_down(const uint8_t *keys, int count, enum binding_id id) {
    if (g_bindings[id].mouse_mask & g_mouse_buttons) {
        return 1;
    }
    for (int i = 0; keys && i < g_bindings[id].scancode_count; ++i) {
        int scancode = g_bindings[id].scancodes[i];
        if (scancode < count && keys[scancode]) {
            return 1;
        }
    }
    return 0;
}

static int g_desktop_game_mode;
static int g_pause_blocked; /* the pause key held across a mode switch counts only once */
static int g_prev_tab;
static int g_prev_fullscreen_key;
static int g_prev_mouse_left;
static float g_look_x;
static float g_look_y;
static float g_swipe_x;
static float g_swipe_y;
static uint64_t g_swipe_idle_ms;

static int desktop_available(void) {
    return g_sdl.GetMouseState && g_sdl.GetKeyboardState && g_sdl.GetMouseFocus &&
           g_sdl.GetWindowSize;
}

static int desktop_key(const uint8_t *keys, int count, int scancode) {
    return scancode < count && keys[scancode];
}

static void desktop_set_game_mode(int enabled) {
    if (enabled == g_desktop_game_mode) {
        return;
    }
    g_desktop_game_mode = enabled;
    g_pause_blocked = 1;
    g_look_x = 0;
    g_look_y = 0;
    if (enabled) {
        input_release_pointer();
    } else {
        touchpad_release_all();
        keyboard_release_all();
        native_input_release();
    }
    if (g_sdl.SetRelativeMouseMode) {
        g_sdl.SetRelativeMouseMode(enabled);
    }
    if (g_sdl.GetRelativeMouseState) {
        g_sdl.GetRelativeMouseState(NULL, NULL);
    }
    fprintf(stderr, "[input] %s mode\n", enabled ? "game (mouse captured)" : "menu (mouse pointer)");
}

static void desktop_update_menu(void *window) {
    int mx = 0, my = 0, ww = 0, wh = 0;
    uint32_t buttons = g_sdl.GetMouseState(&mx, &my);
    g_sdl.GetWindowSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0) {
        return;
    }
    int32_t rect[4];
    present_rect(ww, wh, rect);
    if (rect[2] <= 0 || rect[3] <= 0) {
        return;
    }
    int32_t x = clamp_pointer_x((int32_t)((int64_t)(mx - rect[0]) * window_width() / rect[2]));
    int32_t y = clamp_pointer_y((int32_t)((int64_t)(my - rect[1]) * window_height() / rect[3]));
    if (x != g_pointer_x || y != g_pointer_y) {
        g_pointer_x = x;
        g_pointer_y = y;
        pointer_dispatch_motion();
    }
    int left = (buttons & MOUSE_LEFT) != 0;
    if (left != g_prev_mouse_left) {
        pointer_set_down(left);
        pointer_dispatch_button(0, left ? 1 : 0);
    }
    g_prev_mouse_left = left;
}

static int pause_down(const uint8_t *keys, int count) {
    int down = binding_down(keys, count, BIND_PAUSE);
    if (!down) {
        g_pause_blocked = 0;
    }
    return down && !g_pause_blocked;
}

static int32_t desktop_axis(int negative, int positive) {
    return negative == positive ? 0 : negative ? -32767 : 32767;
}

static void desktop_update_game(const uint8_t *keys, int count, uint64_t dt) {
    int dx = 0, dy = 0;
    if (g_sdl.GetRelativeMouseState) {
        g_sdl.GetRelativeMouseState(&dx, &dy);
    }
    float sensitivity = 12000.0f;
    const char *env = getenv("BOZ_MOUSE_SENS");
    if (env) {
        sensitivity = strtof(env, NULL);
    }
    float scale = dt ? sensitivity / (float)dt : 0.0f;
    g_look_x = g_look_x * 0.5f + (float)dx * scale * 0.5f;
    g_look_y = g_look_y * 0.5f + (float)dy * scale * 0.5f;
    if (g_look_x > 32767.0f) g_look_x = 32767.0f;
    if (g_look_x < -32767.0f) g_look_x = -32767.0f;
    if (g_look_y > 32767.0f) g_look_y = 32767.0f;
    if (g_look_y < -32767.0f) g_look_y = -32767.0f;

    int32_t width = XPERIA_TOUCHPAD_WIDTH;
    int32_t height = XPERIA_TOUCHPAD_HEIGHT;
    int32_t look_center = (width * 4) / 5;
    int32_t look_radius = width - 1 - look_center;
    const char *radius_env = getenv("BOZ_LOOK_RADIUS");
    if (radius_env && atoi(radius_env) > 0 && atoi(radius_env) < look_radius) {
        look_radius = atoi(radius_env);
    }
    touchpad_update_stick(0, desktop_axis(binding_down(keys, count, BIND_MOVE_LEFT),
                                          binding_down(keys, count, BIND_MOVE_RIGHT)),
                          desktop_axis(binding_down(keys, count, BIND_MOVE_FORWARD),
                                       binding_down(keys, count, BIND_MOVE_BACK)),
                          width / 5, height / 2, width / 5, height / 2);
    const char *mode = getenv("BOZ_LOOK_MODE");
    if (mode && strcmp(mode, "swipe") == 0) {
        float gain = 2.0f;
        const char *gain_env = getenv("BOZ_SWIPE_GAIN");
        if (gain_env) {
            gain = strtof(gain_env, NULL);
        }
        if (dx || dy) {
            g_swipe_idle_ms = 0;
            if (!g_touchpad_active[1]) {
                g_swipe_x = (float)look_center;
                g_swipe_y = (float)(height / 2);
                g_touchpad_active[1] = 1;
                g_touchpad_x[1] = look_center;
                g_touchpad_y[1] = height / 2;
                g_touchpad_state[1] = POINTER_STATE_PRESSED;
                touchpad_dispatch_motion(1, g_touchpad_x[1], g_touchpad_y[1]);
                touchpad_dispatch_button(1, S3E_TOUCHPAD_PRESSED, g_touchpad_x[1], g_touchpad_y[1]);
            }
            g_swipe_x += (float)dx * gain;
            g_swipe_y += (float)dy * gain;
            if (g_swipe_x < (float)(width / 2) || g_swipe_x > (float)(width - 1) || g_swipe_y < 0.0f ||
                g_swipe_y > (float)(height - 1)) {
                touchpad_release(1);
            } else {
                g_touchpad_x[1] = (int32_t)g_swipe_x;
                g_touchpad_y[1] = (int32_t)g_swipe_y;
                touchpad_dispatch_motion(1, g_touchpad_x[1], g_touchpad_y[1]);
            }
        } else if (g_touchpad_active[1]) {
            g_swipe_idle_ms += dt;
            if (g_swipe_idle_ms > 120) {
                touchpad_release(1);
            }
        }
    } else {
        touchpad_update_stick(1, (int32_t)g_look_x, (int32_t)g_look_y, look_center, height / 2,
                              look_radius, look_radius);
    }

    game_action_apply(binding_down(keys, count, BIND_SHOOT), &KEYMAP_SHOOT);
    game_action_apply(binding_down(keys, count, BIND_AIM), &KEYMAP_AIM);
    game_action_apply(binding_down(keys, count, BIND_RELOAD), &KEYMAP_RELOAD);
    game_action_apply(binding_down(keys, count, BIND_ACTION) || binding_down(keys, count, BIND_SPRINT),
                      &KEYMAP_ACTION);
    game_action_apply(binding_down(keys, count, BIND_MELEE), &KEYMAP_MELEE);
    game_action_apply(binding_down(keys, count, BIND_GRENADE), &KEYMAP_GRENADE);
    game_action_apply(binding_down(keys, count, BIND_TACTICAL), &KEYMAP_TACTICAL);
    game_action_apply(binding_down(keys, count, BIND_CROUCH), &KEYMAP_CROUCH);
    game_action_apply(binding_down(keys, count, BIND_ALT_FIRE), &KEYMAP_ALT_FIRE);
    game_action_apply(binding_down(keys, count, BIND_SWITCH_WEAPON), &KEYMAP_CHANGE_WEAPON);
    game_action_apply(pause_down(keys, count), &KEYMAP_START);
}

/* Game mode in a Zombies match with native controls: the mouse turns the player directly, the
 * movement keys and buttons go to the game's own input functions (native_input.c). Only pause
 * still goes through the Xperia Play key. */
static void desktop_update_native(const uint8_t *keys, int count) {
    static const struct {
        enum binding_id binding;
        enum native_action action;
    } BUTTONS[] = {
        {BIND_SHOOT, NATIVE_SHOOT},       {BIND_AIM, NATIVE_AIM},
        {BIND_RELOAD, NATIVE_RELOAD},     {BIND_ACTION, NATIVE_USE},
        {BIND_MELEE, NATIVE_MELEE},       {BIND_GRENADE, NATIVE_GRENADE},
        {BIND_TACTICAL, NATIVE_TACTICAL}, {BIND_CROUCH, NATIVE_CROUCH},
        {BIND_ALT_FIRE, NATIVE_FIRE_MODE}, {BIND_SWITCH_WEAPON, NATIVE_SWITCH_WEAPON},
    };
    int dx = 0, dy = 0;
    if (g_sdl.GetRelativeMouseState) {
        g_sdl.GetRelativeMouseState(&dx, &dy);
    }
    native_input_look((float)dx, (float)dy);
    float x = (float)(binding_down(keys, count, BIND_MOVE_RIGHT) - binding_down(keys, count, BIND_MOVE_LEFT));
    float y = (float)(binding_down(keys, count, BIND_MOVE_BACK) - binding_down(keys, count, BIND_MOVE_FORWARD));
    native_input_move(x, y, binding_down(keys, count, BIND_SPRINT) != 0);
    for (size_t i = 0; i < ARRAY_SIZE(BUTTONS); ++i) {
        native_input_action(BUTTONS[i].action, binding_down(keys, count, BUTTONS[i].binding) != 0);
    }
    game_action_apply(pause_down(keys, count), &KEYMAP_START);
}

/* F11 or Alt+Enter switches between a window and borderless fullscreen. */
static void desktop_toggle_fullscreen(void) {
    enum { FULLSCREEN_DESKTOP = 0x00001001u, FULLSCREEN_ANY = 0x00000001u };
    void *window = g_sdl.GetKeyboardFocus ? g_sdl.GetKeyboardFocus() : NULL;
    if (!window || !g_sdl.GetWindowFlags || !g_sdl.SetWindowFullscreen) {
        return;
    }
    int fullscreen = (g_sdl.GetWindowFlags(window) & FULLSCREEN_ANY) != 0;
    if (g_sdl.SetWindowFullscreen(window, fullscreen ? 0 : FULLSCREEN_DESKTOP) != 0) {
        fprintf(stderr, "[input] fullscreen toggle failed: %s\n",
                g_sdl.GetError ? g_sdl.GetError() : "?");
        return;
    }
    fprintf(stderr, "[input] %s\n", fullscreen ? "windowed" : "fullscreen");
}

/* Menu or game mode. With native controls the mode follows the game: game mode (mouse captured)
 * while a match runs unpaused and the window has focus, menu mode (mouse pointer) otherwise, so
 * pausing frees the mouse. The toggle key frees the mouse during a match until pressed again.
 * Without native controls the toggle key switches modes by hand. */
static int g_mouse_freed;

static void desktop_update(uint64_t dt) {
    if (!desktop_available()) {
        return;
    }
    load_bindings();
    void *window = g_sdl.GetMouseFocus();
    int count = 0;
    const uint8_t *keys = g_sdl.GetKeyboardState(&count);
    g_mouse_buttons = g_sdl.GetMouseState(NULL, NULL);
    int fullscreen_key =
        keys && (binding_down(keys, count, BIND_FULLSCREEN) ||
                 (desktop_key(keys, count, SC_RETURN) &&
                  (desktop_key(keys, count, SC_LALT) || desktop_key(keys, count, SC_RALT))));
    if (fullscreen_key && !g_prev_fullscreen_key) {
        desktop_toggle_fullscreen();
    }
    g_prev_fullscreen_key = fullscreen_key;
    int tab = binding_down(keys, count, BIND_TOGGLE_MODE);
    int tab_pressed = tab && !g_prev_tab;
    g_prev_tab = tab;

    enum native_match match = native_input_match();
    if (native_input_available()) {
        if (match == NATIVE_MATCH_NONE) {
            g_mouse_freed = 0;
        } else if (tab_pressed) {
            g_mouse_freed = !g_mouse_freed;
        }
        int focused = g_sdl.GetKeyboardFocus ? g_sdl.GetKeyboardFocus() != NULL : 1;
        desktop_set_game_mode(match != NATIVE_MATCH_NONE && !g_mouse_freed && focused);
    } else if (tab_pressed) {
        desktop_set_game_mode(!g_desktop_game_mode);
    }

    if (g_desktop_game_mode) {
        if (!keys) {
            return;
        }
        if (match == NATIVE_MATCH_ZOMBIES) {
            touchpad_release_all();
            desktop_update_native(keys, count);
        } else {
            native_input_release();
            desktop_update_game(keys, count, dt);
        }
    } else if (window) {
        desktop_update_menu(window);
        /* Escape in the pause menu resumes, as the pause key does on the phone. */
        game_action_apply(keys && pause_down(keys, count), &KEYMAP_START);
    }
}

static void input_close_controller(void) {
    input_release_pointer();
    touchpad_release_all();
    keyboard_release_all();
    input_reset_triggers();
    g_prev_select = 0;
    g_prev_a = 0;
    g_input_last_ms = 0;
    g_hat_mask = 0;

    if (g_controller && g_sdl.GameControllerClose) {
        g_sdl.GameControllerClose(g_controller);
    }
    g_controller = NULL;
    g_joystick = NULL;
}

static void input_refresh_controller(uint64_t now) {
    if (g_controller && !g_sdl.GameControllerGetAttached(g_controller)) {
        fprintf(stderr, "[input] controller disconnected\n");
        input_close_controller();
        g_controller_scan_ms = 0;
        g_last_joystick_count = -1;
    }

    if (!g_controller &&
        (!g_controller_scan_ms || now - g_controller_scan_ms >= CONTROLLER_RESCAN_MS)) {
        g_controller_scan_ms = now;
        input_scan_controllers();
    }
}

/* Code mods: SDL events go to the overlay and to Lua key handlers. While a mod has the overlay
 * take input, the game gets no input at all. SDL_Event layouts are those of SDL 2.0.18+. */
enum {
    SDL_KEYDOWN_EVENT = 0x300,
    SDL_KEYUP_EVENT = 0x301,
    SDL_TEXTINPUT_EVENT = 0x303,
    SDL_MOUSEMOTION_EVENT = 0x400,
    SDL_MOUSEBUTTONDOWN_EVENT = 0x401,
    SDL_MOUSEBUTTONUP_EVENT = 0x402,
    SDL_MOUSEWHEEL_EVENT = 0x403,
    SDL_EVENT_BYTES = 56,
};

static int g_overlay_captured;
static int g_suppress_text;

static void mod_mouse_scale(float *sx, float *sy) {
    *sx = *sy = 1.0f;
    void *window = g_sdl.GetMouseFocus ? g_sdl.GetMouseFocus() : NULL;
    int ww = 0, wh = 0, dw = 0, dh = 0;
    if (window && g_sdl.GetWindowSize) {
        g_sdl.GetWindowSize(window, &ww, &wh);
    }
    if (ww > 0 && wh > 0 && egl_backend_drawable_size(&dw, &dh) && dw > 0 && dh > 0) {
        *sx = (float)dw / (float)ww;
        *sy = (float)dh / (float)wh;
    }
}

static void mod_handle_event(const uint8_t *ev) {
    uint32_t type;
    memcpy(&type, ev, 4);
    struct overlay_event out;
    memset(&out, 0, sizeof(out));
    switch (type) {
    case SDL_KEYDOWN_EVENT:
    case SDL_KEYUP_EVENT: {
        int32_t scancode;
        uint16_t mod;
        memcpy(&scancode, ev + 16, 4);
        memcpy(&mod, ev + 24, 2);
        out.kind = OVERLAY_KEY;
        out.scancode = scancode;
        out.down = type == SDL_KEYDOWN_EVENT;
        out.modifiers = ((mod & 0x00c0) ? 1 : 0) | ((mod & 0x0003) ? 2 : 0) | ((mod & 0x0300) ? 4 : 0) |
                        ((mod & 0x0c00) ? 8 : 0);
        overlay_event(&out);
        const char *name = g_sdl.GetScancodeName ? g_sdl.GetScancodeName(scancode) : NULL;
        if (lua_runtime_key(name, out.down, ev[13] != 0) && out.down) {
            g_suppress_text = 1; /* the key a mod used should not also type its character */
        } else if (out.down) {
            g_suppress_text = 0;
        }
        break;
    }
    case SDL_TEXTINPUT_EVENT: {
        char text[33];
        memcpy(text, ev + 12, 32);
        text[32] = '\0';
        if (g_suppress_text) {
            g_suppress_text = 0;
            break;
        }
        out.kind = OVERLAY_TEXT;
        out.text = text;
        overlay_event(&out);
        break;
    }
    case SDL_MOUSEMOTION_EVENT: {
        int32_t x, y;
        float sx, sy;
        memcpy(&x, ev + 20, 4);
        memcpy(&y, ev + 24, 4);
        mod_mouse_scale(&sx, &sy);
        out.kind = OVERLAY_MOUSE_MOVE;
        out.x = (float)x * sx;
        out.y = (float)y * sy;
        overlay_event(&out);
        break;
    }
    case SDL_MOUSEBUTTONDOWN_EVENT:
    case SDL_MOUSEBUTTONUP_EVENT: {
        static const int buttons[] = {-1, 0, 2, 1}; /* SDL left, middle, right -> ImGui 0, 2, 1 */
        uint8_t button = ev[16];
        int32_t x, y;
        float sx, sy;
        memcpy(&x, ev + 20, 4);
        memcpy(&y, ev + 24, 4);
        mod_mouse_scale(&sx, &sy);
        out.kind = OVERLAY_MOUSE_MOVE;
        out.x = (float)x * sx;
        out.y = (float)y * sy;
        overlay_event(&out);
        if (button >= 1 && button <= 3) {
            out.kind = OVERLAY_MOUSE_BUTTON;
            out.button = buttons[button];
            out.down = type == SDL_MOUSEBUTTONDOWN_EVENT;
            overlay_event(&out);
        }
        break;
    }
    case SDL_MOUSEWHEEL_EVENT: {
        uint32_t direction;
        float px, py;
        memcpy(&direction, ev + 24, 4);
        memcpy(&px, ev + 28, 4);
        memcpy(&py, ev + 32, 4);
        out.kind = OVERLAY_MOUSE_WHEEL;
        out.wheel_x = direction == 1 ? px : -px;
        out.wheel_y = direction == 1 ? -py : py;
        overlay_event(&out);
        break;
    }
    default:
        break;
    }
}

/* Returns true while the overlay owns input (the game gets none this frame). */
static int mod_overlay_input(void) {
    int capturing = overlay_capturing();
    if (capturing != g_overlay_captured) {
        g_overlay_captured = capturing;
        native_input_release();
        touchpad_release_all();
        keyboard_release_all();
        input_release_pointer();
        input_reset_triggers();
        if (g_sdl.SetRelativeMouseMode) {
            g_sdl.SetRelativeMouseMode(capturing ? 0 : g_desktop_game_mode);
        }
        if (g_sdl.GetRelativeMouseState) {
            g_sdl.GetRelativeMouseState(NULL, NULL);
        }
    }
    return capturing;
}

/* The crosshair pointer is drawn for controllers, and for the mouse only when [display]
 * software_cursor is on (otherwise the system pointer shows). */
int input_draw_software_cursor(void) {
    static int setting = -1;
    if (setting < 0) {
        const char *value = getenv("BOZ_SOFTWARE_CURSOR");
        setting = value && value[0] && strcmp(value, "0") != 0;
    }
    return g_cursor_active && (g_controller != NULL || setting);
}

void input_pump(void) {
    if (g_input_pumping) {
        return;
    }
    g_input_pumping = 1;

    input_open();
    if (!g_sdl_initialized) {
        goto out;
    }

    /* The game polls controller state and has no SDL event consumer. */
    g_sdl.PumpEvents();
    if (g_sdl.HasEvent && g_sdl.HasEvent(SDL_QUIT_EVENT)) {
        static uint64_t quit_ms;
        if (!quit_ms) {
            quit_ms = monotonic_ms();
            fprintf(stderr, "[input] window close requested\n");
            s3eDeviceRequestQuit();
        } else if (monotonic_ms() - quit_ms > 3000) {
            fprintf(stderr, "[input] game did not exit after close request, exiting\n");
            _Exit(0);
        }
    }
    if (g_sdl.PollEvent) {
        uint8_t event[SDL_EVENT_BYTES + 8];
        while (g_sdl.PollEvent(event)) {
            mod_handle_event(event);
        }
    } else {
        g_sdl.FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    }

    uint64_t now = monotonic_ms();
    if (mod_overlay_input()) {
        g_input_last_ms = now;
        goto out;
    }
    input_refresh_controller(now);
    if (!g_input_last_ms) {
        g_input_last_ms = now;
    }
    uint64_t dt = now - g_input_last_ms;
    g_input_last_ms = now;
    if (dt > 50) {
        dt = 50;
    }
    if (!g_controller) {
        desktop_update(dt);
        goto out;
    }
    g_sdl.GameControllerUpdate();
    g_hat_mask = input_current_hat_mask();

    int select = input_button(SDL_BUTTON_BACK);
    if (select && !g_prev_select) {
        if (g_cursor_active) {
            input_release_pointer();
        } else {
            touchpad_release_all();
            keyboard_release_all();
            input_reset_triggers();
        }
        g_cursor_active = !g_cursor_active;
    }
    g_prev_select = select;

    if (g_cursor_active) {
        touchpad_release_all();
        keyboard_release_all();
        input_reset_triggers();
        input_update_cursor(dt);
    } else {
        input_release_pointer();
        input_update_game_touchpads();
        input_update_game_keys();
    }

out:
    g_input_pumping = 0;
}

void input_shutdown(void) {
    input_close_controller();
    if (g_sdl2) {
        if (g_sdl_initialized && g_sdl.QuitSubSystem) {
            g_sdl.QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        }
        plat_lib_close(g_sdl2);
        g_sdl2 = NULL;
    }
    memset(&g_sdl, 0, sizeof(g_sdl));
    g_sdl_tried = 0;
    g_sdl_initialized = 0;
    g_input_pumping = 0;
    g_controller_scan_ms = 0;
    g_last_joystick_count = -1;
}

int32_t s3eKeyboardRegister(uint32_t id, void *callback, void *user_data) {
    struct keyboard_callback_slot *free_slot = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(g_keyboard_callbacks); ++i) {
        struct keyboard_callback_slot *slot = &g_keyboard_callbacks[i];
        if (slot->callback == callback && slot->id == id) {
            slot->user_data = user_data;
            return 0;
        }
        if (!slot->callback && !free_slot) {
            free_slot = slot;
        }
    }
    if (free_slot) {
        free_slot->id = id;
        free_slot->callback = callback;
        free_slot->user_data = user_data;
    }
    return 0;
}

int32_t s3eKeyboardUnRegister(uint32_t id, void *callback) {
    for (size_t i = 0; i < ARRAY_SIZE(g_keyboard_callbacks); ++i) {
        struct keyboard_callback_slot *slot = &g_keyboard_callbacks[i];
        if (slot->callback && slot->id == id && (!callback || callback == slot->callback)) {
            slot->callback = NULL;
            slot->user_data = NULL;
        }
    }
    return 0;
}

int32_t s3eKeyboardUpdate(void) {
    input_pump();
    keyboard_publish();
    dispatch_due_timers();
    return 0;
}

int32_t s3eKeyboardGetState(uint32_t key) {
    uint32_t target = keyboard_abs_target(key);
    return target < KEYBOARD_KEY_COUNT ? g_keyboard_published[target] : 0;
}

int32_t s3eKeyboardAnyKey(void) {
    for (size_t i = 0; i < ARRAY_SIZE(g_keyboard_published); ++i) {
        if (g_keyboard_published[i] & KEY_STATE_PRESSED) {
            return (int32_t)i;
        }
    }
    return 0;
}

int32_t s3eKeyboardGetInt(uint32_t key) {
    switch (key) {
    case 0:
    case 2:
        return 1;
    case 1:
    case 4:
    case 5:
    case 6:
        return 0;
    default:
        return -1;
    }
}

int32_t s3eKeyboardSetInt(uint32_t key, int32_t value) {
    (void)key;
    (void)value;
    return 0;
}

const char *s3eKeyboardGetDisplayName(uint32_t key) {
    switch (key) {
    case BINDING_KEY_SHOOT:
        return "BindingShoot";
    case BINDING_KEY_CHANGE_WEAPON:
        return "BindingChangeWeapon";
    case BINDING_KEY_CROUCH_PRONE:
        return "BindingCrouchProne";
    case BINDING_KEY_RELOAD:
        return "BindingReload";
    case BINDING_KEY_MELEE:
        return "BindingMelee";
    case BINDING_KEY_ALTERNATE_FIRE:
        return "BindingAlternateFire";
    case BINDING_KEY_AIM:
        return "BindingAim";
    case BINDING_KEY_THROW_GRENADE:
        return "BindingThrowGrenade";
    case BINDING_KEY_TACTICAL_GRENADE:
        return "BindingTacticalGrenade";
    case BINDING_KEY_ACTION_SPRINT:
        return "BindingActionSprint";
    case BINDING_KEY_TOGGLE_FREE_MODE:
        return "BindingToggleFreeMode";
    case XPERIA_KEY_ALTERNATE_FIRE:
        return "XperiaAlternateFire";
    case XPERIA_KEY_TACTICAL_GRENADE:
        return "XperiaTacticalGrenade";
    case XPERIA_KEY_CHANGE_WEAPON:
        return "XperiaChangeWeapon";
    case XPERIA_KEY_CROUCH_PRONE:
        return "XperiaCrouchProne";
    case XPERIA_KEY_AIM:
        return "XperiaAim";
    case XPERIA_KEY_SHOOT:
        return "XperiaShoot";
    case XPERIA_KEY_ACTION_SPRINT:
        return "XperiaActionSprint";
    case XPERIA_KEY_MELEE:
        return "XperiaMelee";
    case XPERIA_KEY_THROW_GRENADE:
        return "XperiaThrowGrenade";
    case XPERIA_KEY_RELOAD:
        return "XperiaReload";
    case XPERIA_KEY_PAUSE:
        return "XperiaPause";
    case S3E_KEY_ABS_GAME_A:
        return "KeyAbsGameA";
    case S3E_KEY_ABS_GAME_B:
        return "KeyAbsGameB";
    case S3E_KEY_ABS_GAME_C:
        return "KeyAbsGameC";
    case S3E_KEY_ABS_GAME_D:
        return "KeyAbsGameD";
    case S3E_KEY_ABS_UP:
        return "KeyAbsUp";
    case S3E_KEY_ABS_DOWN:
        return "KeyAbsDown";
    case S3E_KEY_ABS_LEFT:
        return "KeyAbsLeft";
    case S3E_KEY_ABS_RIGHT:
        return "KeyAbsRight";
    case S3E_KEY_ABS_OK:
        return "KeyAbsOk";
    case S3E_KEY_ABS_ASK:
        return "KeyAbsASK";
    case S3E_KEY_ABS_BSK:
        return "KeyAbsBSK";
    default:
        return "";
    }
}

void s3eKeyboardClearState(void) {
    memset(g_keyboard_live, 0, sizeof(g_keyboard_live));
    memset(g_keyboard_published, 0, sizeof(g_keyboard_published));
    input_reset_triggers();
}

int32_t s3ePointerRegister(uint32_t id, void *callback, void *user_data) {
    if (id < ARRAY_SIZE(g_pointer_callbacks)) {
        g_pointer_callbacks[id].callback = callback;
        g_pointer_callbacks[id].user_data = user_data;
    }
    return 0;
}

int32_t s3ePointerUnRegister(uint32_t id, void *callback) {
    if (id < ARRAY_SIZE(g_pointer_callbacks) &&
        (!callback || callback == g_pointer_callbacks[id].callback)) {
        g_pointer_callbacks[id].callback = NULL;
        g_pointer_callbacks[id].user_data = NULL;
    }
    return 0;
}

int32_t s3ePointerUpdate(void) {
    pointer_clear_transitions();
    input_pump();
    dispatch_due_timers();
    return 0;
}

int32_t s3ePointerGetInt(uint32_t key) {
    input_pump();
    switch (key) {
    case 0:
        return 1;
    case 1:
        return g_pointer_x;
    case 2:
        return g_pointer_y;
    default:
        return 0;
    }
}

int32_t s3ePointerSetInt(uint32_t key, int32_t value) {
    (void)key;
    (void)value;
    return 0;
}

int32_t s3ePointerGetState(uint32_t button) {
    input_pump();
    return button < sizeof(g_pointer_states) ? g_pointer_states[button] : 0;
}

int32_t s3ePointerGetX(void) {
    input_pump();
    return g_pointer_x;
}

int32_t s3ePointerGetY(void) {
    input_pump();
    return g_pointer_y;
}

int32_t s3ePointerGetTouchState(uint32_t touch_id) {
    input_pump();
    if (g_cursor_active) {
        return touch_id == 0 ? g_pointer_states[0] : 0;
    }
    return 0;
}

int32_t s3ePointerGetTouchX(uint32_t touch_id) {
    input_pump();
    if (g_cursor_active) {
        return touch_id == 0 ? g_pointer_x : 0;
    }
    return 0;
}

int32_t s3ePointerGetTouchY(uint32_t touch_id) {
    input_pump();
    if (g_cursor_active) {
        return touch_id == 0 ? g_pointer_y : 0;
    }
    return 0;
}

int32_t s3ePointerGetPressure(uint32_t button) {
    input_pump();
    return button == 0 && g_pointer_down ? 1 : 0;
}

int32_t s3ePointerGetTouchPressure(uint32_t touch_id) {
    input_pump();
    if (g_cursor_active) {
        return touch_id == 0 && g_pointer_down ? 1 : 0;
    }
    return 0;
}

int32_t s3ePointerGetError(void) {
    return 0;
}

const char *s3ePointerGetErrorString(void) {
    return "S3E_POINTER_ERR_NONE";
}
