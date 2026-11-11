#include "s3e_host_internal.h"

#include <assert.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

enum {
    TEST_SDL_BUTTON_A = 0,
    TEST_SDL_BUTTON_BACK = 4,
    TEST_SDL_BUTTON_RIGHTSHOULDER = 10,
    TEST_SDL_AXIS_LEFTX = 0,
    TEST_SDL_AXIS_RIGHTX = 2,
    TEST_SDL_AXIS_TRIGGERRIGHT = 5,
    TEST_XPERIA_KEY_SHOOT = 75,
    TEST_KEY_STATE_DOWN = 1,
    TEST_KEY_STATE_PRESSED = 2,
    TEST_KEY_STATE_RELEASED = 4,
};

struct keyboard_event_log {
    struct s3e_keyboard_event events[32];
    size_t count;
};

enum touchpad_event_type {
    TOUCHPAD_EVENT_PRESS,
    TOUCHPAD_EVENT_MOTION,
};

struct touchpad_event {
    enum touchpad_event_type type;
    int32_t id;
    int32_t x;
    int32_t y;
};

struct touchpad_event_log {
    struct touchpad_event events[8];
    size_t count;
};

struct pointer_event_log {
    int event_ids[8];
    int32_t x[8];
    int32_t y[8];
    size_t count;
};

struct surface_geometry g_surface = {0, 0, 640, 480};
struct callback_slot g_pointer_callbacks[4];
struct callback_slot g_touchpad_callbacks[8];
struct keyboard_callback_slot g_keyboard_callbacks[16];
int32_t g_pointer_x = 110;
int32_t g_pointer_y = 90;
int g_pointer_down;
uint8_t g_pointer_states[5];
int g_cursor_active;

static uint8_t g_fake_buttons[15];
static int16_t g_fake_axes[6];
static int g_fake_joystick_count = 1;
static int g_fake_controller_tokens[4];
static uint8_t g_fake_controller_mapped[4];
static uint8_t g_fake_controller_attached[4];
static uint8_t g_fake_controller_openable[4];
static int g_fake_opened_index;
static int g_fake_pump_count;
static int g_fake_flush_count;
static uint64_t g_fake_now;
static struct keyboard_event_log g_key_events;
static struct keyboard_event_log g_character_events;
static struct touchpad_event_log g_touchpad_events;
static struct pointer_event_log g_pointer_events;

static int fake_sdl_init(uint32_t flags) {
    (void)flags;
    return 0;
}

static void fake_sdl_quit(uint32_t flags) {
    (void)flags;
}

static void fake_pump_events(void) {
    ++g_fake_pump_count;
}

static void fake_flush_events(uint32_t min_type, uint32_t max_type) {
    assert(min_type == 0);
    assert(max_type == 0xffff);
    ++g_fake_flush_count;
}

static int fake_num_joysticks(void) {
    return g_fake_joystick_count;
}

static int fake_is_game_controller(int joystick_index) {
    return joystick_index >= 0 && joystick_index < g_fake_joystick_count &&
           g_fake_controller_mapped[joystick_index];
}

static void *fake_game_controller_open(int joystick_index) {
    if (!fake_is_game_controller(joystick_index) || !g_fake_controller_openable[joystick_index] ||
        !g_fake_controller_attached[joystick_index]) {
        return NULL;
    }
    g_fake_opened_index = joystick_index;
    return &g_fake_controller_tokens[joystick_index];
}

static int fake_controller_index(void *controller) {
    for (size_t i = 0; i < ARRAY_SIZE(g_fake_controller_tokens); ++i) {
        if (controller == &g_fake_controller_tokens[i]) {
            return (int)i;
        }
    }
    assert(0 && "unknown fake controller");
    return -1;
}

static void fake_game_controller_close(void *controller) {
    (void)fake_controller_index(controller);
}

static void *fake_game_controller_get_joystick(void *controller) {
    (void)fake_controller_index(controller);
    return controller;
}

static int fake_game_controller_get_attached(void *controller) {
    return g_fake_controller_attached[fake_controller_index(controller)];
}

static const char *fake_joystick_name_for_index(int joystick_index) {
    assert(joystick_index >= 0 && joystick_index < g_fake_joystick_count);
    return joystick_index ? "fake controller 2" : "fake controller 1";
}

static int fake_joystick_num_hats(void *joystick) {
    (void)fake_controller_index(joystick);
    return 0;
}

static uint8_t fake_joystick_get_hat(void *joystick, int hat) {
    (void)fake_controller_index(joystick);
    (void)hat;
    return 0;
}

static void fake_game_controller_update(void) {}

static int16_t fake_game_controller_get_axis(void *controller, int axis) {
    (void)fake_controller_index(controller);
    assert(axis >= 0 && axis < (int)ARRAY_SIZE(g_fake_axes));
    return g_fake_axes[axis];
}

static uint8_t fake_game_controller_get_button(void *controller, int button) {
    (void)fake_controller_index(controller);
    assert(button >= 0 && button < (int)ARRAY_SIZE(g_fake_buttons));
    return g_fake_buttons[button];
}

/* Finger events for the touch-screen test, handed out by SDL_PollEvent. */
static uint8_t g_fake_events[8][64];
static int g_fake_event_count, g_fake_event_next;
static int g_fake_window_token;
static int g_fake_poll; /* SDL_PollEvent exists (the touch test); otherwise events are flushed */

static void fake_finger(uint32_t type, int64_t finger, float x, float y) {
    assert(g_fake_event_count < 8);
    uint8_t *ev = g_fake_events[g_fake_event_count++];
    memset(ev, 0, 64);
    memcpy(ev, &type, 4);
    memcpy(ev + 16, &finger, 8);
    memcpy(ev + 24, &x, 4);
    memcpy(ev + 28, &y, 4);
}

static int fake_poll_event(void *event) {
    if (g_fake_event_next == g_fake_event_count) {
        return 0;
    }
    memcpy(event, g_fake_events[g_fake_event_next++], 56);
    return 1;
}

static void *fake_keyboard_focus(void) {
    return &g_fake_window_token;
}

static void fake_window_size(void *window, int *w, int *h) {
    (void)window;
    *w = 640;
    *h = 480;
}

static const char *g_lua_keys[4];
static int g_lua_key_count;

bool lua_runtime_key(const char *name, bool down, bool repeat) {
    (void)repeat;
    if (down && g_lua_key_count < 4) {
        g_lua_keys[g_lua_key_count++] = name;
    }
    return false;
}

static const char *fake_sdl_error(void) {
    return "fake input error";
}

void *open_first(const char *const *names) {
    assert(names && names[0]);
    return &g_fake_controller_tokens[0];
}

void *dlsym(void *handle, const char *name) {
    assert(handle == &g_fake_controller_tokens[0]);
#define TEST_SYMBOL(symbol, function)                                                              \
    if (strcmp(name, symbol) == 0) {                                                               \
        return (void *)(uintptr_t)&function;                                                       \
    }
    TEST_SYMBOL("SDL_InitSubSystem", fake_sdl_init)
    TEST_SYMBOL("SDL_QuitSubSystem", fake_sdl_quit)
    TEST_SYMBOL("SDL_PumpEvents", fake_pump_events)
    TEST_SYMBOL("SDL_FlushEvents", fake_flush_events)
    TEST_SYMBOL("SDL_NumJoysticks", fake_num_joysticks)
    TEST_SYMBOL("SDL_IsGameController", fake_is_game_controller)
    TEST_SYMBOL("SDL_JoystickNameForIndex", fake_joystick_name_for_index)
    TEST_SYMBOL("SDL_GameControllerNameForIndex", fake_joystick_name_for_index)
    TEST_SYMBOL("SDL_GameControllerOpen", fake_game_controller_open)
    TEST_SYMBOL("SDL_GameControllerClose", fake_game_controller_close)
    TEST_SYMBOL("SDL_GameControllerGetAttached", fake_game_controller_get_attached)
    TEST_SYMBOL("SDL_GameControllerGetJoystick", fake_game_controller_get_joystick)
    TEST_SYMBOL("SDL_JoystickNumHats", fake_joystick_num_hats)
    TEST_SYMBOL("SDL_JoystickGetHat", fake_joystick_get_hat)
    TEST_SYMBOL("SDL_GameControllerUpdate", fake_game_controller_update)
    TEST_SYMBOL("SDL_GameControllerGetAxis", fake_game_controller_get_axis)
    TEST_SYMBOL("SDL_GameControllerGetButton", fake_game_controller_get_button)
    TEST_SYMBOL("SDL_GetError", fake_sdl_error)
    if (g_fake_poll) {
        TEST_SYMBOL("SDL_PollEvent", fake_poll_event)
    }
    TEST_SYMBOL("SDL_GetKeyboardFocus", fake_keyboard_focus)
    TEST_SYMBOL("SDL_GetWindowSize", fake_window_size)
#undef TEST_SYMBOL
    return NULL;
}

int dlclose(void *handle) {
    assert(handle == &g_fake_controller_tokens[0]);
    return 0;
}

uint64_t monotonic_ms(void) {
    g_fake_now += 16;
    return g_fake_now;
}

void dispatch_due_timers(void) {}

static int32_t record_keyboard_event(void *system_data, void *user_data) {
    struct keyboard_event_log *log = user_data;
    assert(log->count < ARRAY_SIZE(log->events));
    log->events[log->count++] = *(const struct s3e_keyboard_event *)system_data;
    return 0;
}

static int32_t record_touchpad_button(void *system_data, void *user_data) {
    struct touchpad_event_log *log = user_data;
    const struct s3e_touchpad_button_event *event = system_data;
    assert(log->count < ARRAY_SIZE(log->events));
    assert(event->pressed == 1);
    log->events[log->count++] = (struct touchpad_event){
        .type = TOUCHPAD_EVENT_PRESS,
        .id = event->id,
        .x = event->x,
        .y = event->y,
    };
    return 0;
}

static int32_t record_touchpad_motion(void *system_data, void *user_data) {
    struct touchpad_event_log *log = user_data;
    const struct s3e_touchpad_motion_event *event = system_data;
    assert(log->count < ARRAY_SIZE(log->events));
    log->events[log->count++] = (struct touchpad_event){
        .type = TOUCHPAD_EVENT_MOTION,
        .id = event->id,
        .x = event->x,
        .y = event->y,
    };
    return 0;
}

static int32_t record_pointer_button(void *system_data, void *user_data) {
    struct pointer_event_log *log = user_data;
    const struct s3e_pointer_button_event *event = system_data;
    assert(log->count < ARRAY_SIZE(log->event_ids));
    assert(event->pressed == 1);
    log->event_ids[log->count] = 0;
    log->x[log->count] = event->x;
    log->y[log->count++] = event->y;
    return 0;
}

static int32_t record_pointer_motion(void *system_data, void *user_data) {
    struct pointer_event_log *log = user_data;
    const struct s3e_pointer_motion_event *event = system_data;
    assert(log->count < ARRAY_SIZE(log->event_ids));
    log->event_ids[log->count] = 1;
    log->x[log->count] = event->x;
    log->y[log->count++] = event->y;
    return 0;
}

static void reset_input(void) {
    memset(g_keyboard_callbacks, 0, sizeof(g_keyboard_callbacks));
    memset(g_pointer_callbacks, 0, sizeof(g_pointer_callbacks));
    memset(g_touchpad_callbacks, 0, sizeof(g_touchpad_callbacks));
    input_shutdown();

    memset(g_fake_buttons, 0, sizeof(g_fake_buttons));
    memset(g_fake_axes, 0, sizeof(g_fake_axes));
    memset(g_pointer_states, 0, sizeof(g_pointer_states));
    memset(&g_key_events, 0, sizeof(g_key_events));
    memset(&g_character_events, 0, sizeof(g_character_events));
    memset(&g_touchpad_events, 0, sizeof(g_touchpad_events));
    memset(&g_pointer_events, 0, sizeof(g_pointer_events));
    memset(g_fake_controller_mapped, 0, sizeof(g_fake_controller_mapped));
    memset(g_fake_controller_attached, 0, sizeof(g_fake_controller_attached));
    memset(g_fake_controller_openable, 0, sizeof(g_fake_controller_openable));
    g_fake_joystick_count = 1;
    g_fake_controller_mapped[0] = 1;
    g_fake_controller_attached[0] = 1;
    g_fake_controller_openable[0] = 1;
    g_fake_opened_index = -1;
    g_fake_pump_count = 0;
    g_fake_flush_count = 0;
    g_fake_now = 0;
    g_pointer_down = 0;
    g_cursor_active = 0;
    s3eKeyboardClearState();

    assert(s3eKeyboardRegister(0, (void *)(uintptr_t)&record_keyboard_event, &g_key_events) == 0);
    assert(s3eKeyboardRegister(1, (void *)(uintptr_t)&record_keyboard_event, &g_character_events) ==
           0);
}

static void assert_event(const struct keyboard_event_log *log, size_t index, int pressed) {
    assert(index < log->count);
    assert(log->events[index].key == TEST_XPERIA_KEY_SHOOT);
    assert(log->events[index].pressed == pressed);
}

static void test_skips_unmapped_joysticks(void) {
    reset_input();

    g_fake_joystick_count = 2;
    g_fake_controller_mapped[0] = 0;
    g_fake_controller_mapped[1] = 1;
    g_fake_controller_attached[1] = 1;
    g_fake_controller_openable[1] = 1;

    input_pump();
    assert(g_fake_opened_index == 1);
    assert(g_fake_pump_count == 1);
    assert(g_fake_flush_count == 1);
}

static void test_retries_controller_discovery(void) {
    reset_input();

    g_fake_joystick_count = 0;
    input_pump();
    assert(g_fake_opened_index == -1);

    g_fake_joystick_count = 1;
    for (int i = 0; i < 64 && g_fake_opened_index < 0; ++i) {
        input_pump();
    }
    assert(g_fake_opened_index == 0);
}

static void test_disconnect_releases_input_and_reconnects(void) {
    reset_input();

    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 1;
    input_pump();
    assert(g_fake_opened_index == 0);
    assert(g_key_events.count == 1);

    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 0;
    g_fake_controller_attached[0] = 0;
    g_fake_joystick_count = 2;
    g_fake_controller_mapped[1] = 1;
    g_fake_controller_attached[1] = 1;
    g_fake_controller_openable[1] = 1;
    input_pump();

    assert(g_fake_opened_index == 1);
    assert(g_key_events.count == 2);
    assert_event(&g_key_events, 1, 0);

    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 1;
    input_pump();
    assert(g_key_events.count == 3);
    assert_event(&g_key_events, 2, 1);
}

static void test_first_hold_publishes_down_until_release(void) {
    reset_input();

    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 1;
    input_pump();
    assert(g_key_events.count == 1);
    assert(g_character_events.count == 0);
    assert_event(&g_key_events, 0, 1);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) == 0);

    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) ==
           (TEST_KEY_STATE_DOWN | TEST_KEY_STATE_PRESSED));
    assert(s3eKeyboardAnyKey() == TEST_XPERIA_KEY_SHOOT);

    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) == TEST_KEY_STATE_DOWN);
    assert(s3eKeyboardAnyKey() == 0);
    assert(g_key_events.count == 1);

    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 0;
    input_pump();
    assert(g_key_events.count == 2);
    assert_event(&g_key_events, 1, 0);
    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) == TEST_KEY_STATE_RELEASED);
    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) == 0);
}

static void test_overlapping_shoot_sources_release_once(void) {
    reset_input();

    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 1;
    input_pump();
    g_fake_axes[TEST_SDL_AXIS_TRIGGERRIGHT] = 20000;
    input_pump();
    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 0;
    input_pump();
    assert(g_key_events.count == 1);

    g_fake_axes[TEST_SDL_AXIS_TRIGGERRIGHT] = 12000;
    input_pump();
    assert(g_key_events.count == 2);
    assert_event(&g_key_events, 1, 0);
}

static void test_trigger_hysteresis_ignores_threshold_jitter(void) {
    reset_input();

    g_fake_axes[TEST_SDL_AXIS_TRIGGERRIGHT] = 17000;
    input_pump();
    assert(g_key_events.count == 1);

    g_fake_axes[TEST_SDL_AXIS_TRIGGERRIGHT] = 15000;
    input_pump();
    g_fake_axes[TEST_SDL_AXIS_TRIGGERRIGHT] = 13000;
    input_pump();
    g_fake_axes[TEST_SDL_AXIS_TRIGGERRIGHT] = 16500;
    input_pump();
    assert(g_key_events.count == 1);

    g_fake_axes[TEST_SDL_AXIS_TRIGGERRIGHT] = 12000;
    input_pump();
    assert(g_key_events.count == 2);
    assert_event(&g_key_events, 1, 0);
}

static void test_opposite_edges_survive_until_publication(void) {
    reset_input();

    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 1;
    input_pump();
    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 0;
    input_pump();
    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) ==
           (TEST_KEY_STATE_PRESSED | TEST_KEY_STATE_RELEASED));

    reset_input();
    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 1;
    input_pump();
    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardUpdate() == 0);
    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 0;
    input_pump();
    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 1;
    input_pump();
    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) ==
           (TEST_KEY_STATE_DOWN | TEST_KEY_STATE_PRESSED | TEST_KEY_STATE_RELEASED));
}

static void test_cursor_mode_releases_and_reacquires_held_action(void) {
    reset_input();

    g_fake_buttons[TEST_SDL_BUTTON_RIGHTSHOULDER] = 1;
    input_pump();
    assert(s3eKeyboardUpdate() == 0);

    g_fake_buttons[TEST_SDL_BUTTON_BACK] = 1;
    input_pump();
    assert(g_cursor_active == 1);
    assert(g_key_events.count == 2);
    assert_event(&g_key_events, 1, 0);
    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) == TEST_KEY_STATE_RELEASED);

    g_fake_buttons[TEST_SDL_BUTTON_BACK] = 0;
    input_pump();
    g_fake_buttons[TEST_SDL_BUTTON_BACK] = 1;
    input_pump();
    assert(g_cursor_active == 0);
    assert(g_key_events.count == 3);
    assert_event(&g_key_events, 2, 1);
    assert(s3eKeyboardUpdate() == 0);
    assert(s3eKeyboardGetState(TEST_XPERIA_KEY_SHOOT) ==
           (TEST_KEY_STATE_DOWN | TEST_KEY_STATE_PRESSED));
}

static void test_touchpad_initial_position_precedes_press(void) {
    reset_input();

    g_touchpad_callbacks[0].callback = (void *)(uintptr_t)&record_touchpad_button;
    g_touchpad_callbacks[0].user_data = &g_touchpad_events;
    g_touchpad_callbacks[1].callback = (void *)(uintptr_t)&record_touchpad_motion;
    g_touchpad_callbacks[1].user_data = &g_touchpad_events;
    g_fake_axes[TEST_SDL_AXIS_RIGHTX] = 16000;

    input_pump();

    assert(g_touchpad_events.count == 2);
    assert(g_touchpad_events.events[0].type == TOUCHPAD_EVENT_MOTION);
    assert(g_touchpad_events.events[0].id == 1);
    assert(g_touchpad_events.events[1].type == TOUCHPAD_EVENT_PRESS);
    assert(g_touchpad_events.events[1].id == 1);
    assert(g_touchpad_events.events[0].x == g_touchpad_events.events[1].x);
    assert(g_touchpad_events.events[0].y == g_touchpad_events.events[1].y);

    g_fake_axes[TEST_SDL_AXIS_RIGHTX] = 20000;
    input_pump();

    assert(g_touchpad_events.count == 3);
    assert(g_touchpad_events.events[2].type == TOUCHPAD_EVENT_MOTION);
    assert(g_touchpad_events.events[2].id == 1);
    assert(g_touchpad_events.events[0].x != g_touchpad_events.events[2].x);
    assert(g_touchpad_events.events[0].y == g_touchpad_events.events[2].y);
}

static void test_cursor_position_precedes_press(void) {
    reset_input();

    g_pointer_callbacks[0].callback = (void *)(uintptr_t)&record_pointer_button;
    g_pointer_callbacks[0].user_data = &g_pointer_events;
    g_pointer_callbacks[1].callback = (void *)(uintptr_t)&record_pointer_motion;
    g_pointer_callbacks[1].user_data = &g_pointer_events;
    g_fake_buttons[TEST_SDL_BUTTON_BACK] = 1;
    input_pump();
    g_fake_buttons[TEST_SDL_BUTTON_BACK] = 0;
    g_fake_buttons[TEST_SDL_BUTTON_A] = 1;
    g_fake_axes[TEST_SDL_AXIS_LEFTX] = 20000;
    g_fake_now += 16;

    input_pump();

    assert(g_pointer_events.count == 2);
    assert(g_pointer_events.event_ids[0] == 1);
    assert(g_pointer_events.event_ids[1] == 0);
    assert(g_pointer_events.x[0] == g_pointer_events.x[1]);
    assert(g_pointer_events.y[0] == g_pointer_events.y[1]);
}

/* Touch screens: each finger is its own s3e touch (the game's multi-touch controls), the first
 * also moves the polled pointer, and three fingers press F1 for mods. */
struct touch_log {
    int32_t id[8], pressed[8], x[8], y[8];
    int motion[8];
    size_t count;
};
static struct touch_log g_touches;

static int32_t record_touch(void *system_data, void *user_data) {
    const struct s3e_pointer_touch_event *event = system_data;
    struct touch_log *log = user_data;
    assert(log->count < 8);
    log->id[log->count] = event->touch_id;
    log->pressed[log->count] = event->pressed;
    log->x[log->count] = event->x;
    log->y[log->count] = event->y;
    log->motion[log->count++] = 0;
    return 0;
}

static int32_t record_touch_motion(void *system_data, void *user_data) {
    const struct s3e_pointer_touch_motion_event *event = system_data;
    struct touch_log *log = user_data;
    assert(log->count < 8);
    log->id[log->count] = event->touch_id;
    log->x[log->count] = event->x;
    log->y[log->count] = event->y;
    log->motion[log->count++] = 1;
    return 0;
}

static void test_touch_screen_fingers(void) {
    g_fake_poll = 1;
    reset_input();
    assert(s3ePointerGetInt(4) == 1); /* multi-touch available */
    g_pointer_callbacks[2].callback = (void *)(uintptr_t)&record_touch;
    g_pointer_callbacks[2].user_data = &g_touches;
    g_pointer_callbacks[3].callback = (void *)(uintptr_t)&record_touch_motion;
    g_pointer_callbacks[3].user_data = &g_touches;
    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x700, 71, 0.25f, 0.5f); /* left thumb down */
    fake_finger(0x700, 99, 0.75f, 0.5f); /* right thumb down */
    fake_finger(0x702, 71, 0.5f, 0.25f); /* left thumb moves */
    fake_finger(0x701, 99, 0.75f, 0.5f); /* right thumb up */
    input_pump();

    assert(g_touches.count == 4);
    assert(g_touches.id[0] == 0 && g_touches.pressed[0] == 1 && !g_touches.motion[0]);
    assert(g_touches.x[0] == 160 && g_touches.y[0] == 240);
    assert(g_touches.id[1] == 1 && g_touches.pressed[1] == 1);
    assert(g_touches.x[1] == 480 && g_touches.y[1] == 240);
    assert(g_touches.id[2] == 0 && g_touches.motion[2] && g_touches.x[2] == 320 && g_touches.y[2] == 120);
    assert(g_touches.id[3] == 1 && g_touches.pressed[3] == 0);
    assert(g_pointer_x == 320 && g_pointer_y == 120); /* the first finger */
    s3ePointerUpdate(); /* the polled press shows a frame after the move */
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 2);

    /* Three fingers held for a second: F1 for mods, once. Slot 1 is free again and is reused. */
    g_fake_event_count = g_fake_event_next = 0;
    g_touches.count = 0;
    fake_finger(0x700, 5, 0.5f, 0.5f);
    fake_finger(0x700, 6, 0.6f, 0.5f);
    input_pump();
    assert(g_touches.id[0] == 1 && g_touches.id[1] == 2);
    assert(g_lua_key_count == 0); /* not yet: a quick three-finger touch happens in play */
    for (int i = 0; i < 100; ++i) {
        g_fake_event_count = g_fake_event_next = 0;
        input_pump();
    }
    assert(g_lua_key_count == 1 && strcmp(g_lua_keys[0], "F1") == 0);
}

/* The menus: pointer motion and button callbacks, in order. */
static char g_pointer_log[16];
static int32_t g_pointer_log_x, g_pointer_log_y;
static size_t g_pointer_log_count;

static int32_t log_menu_button(void *system_data, void *user_data) {
    const struct s3e_pointer_button_event *event = system_data;
    (void)user_data;
    assert(g_pointer_log_count < sizeof(g_pointer_log) - 1);
    g_pointer_log[g_pointer_log_count++] = event->pressed ? 'D' : 'U';
    g_pointer_log_x = event->x;
    g_pointer_log_y = event->y;
    return 0;
}

static int32_t log_menu_motion(void *system_data, void *user_data) {
    const struct s3e_pointer_motion_event *event = system_data;
    (void)user_data;
    assert(g_pointer_log_count < sizeof(g_pointer_log) - 1);
    g_pointer_log[g_pointer_log_count++] = 'M';
    g_pointer_log_x = event->x;
    g_pointer_log_y = event->y;
    return 0;
}

/* Menus: the first finger moves the pointer (motion, then button events, as a mouse click), and
 * the polled pointer shows the new position unpressed for a frame before PRESSED (the Flash
 * menus press what the pointer was over). Each press and release lasts a whole frame. */
static void test_touch_tap_hovers_then_presses(void) {
    g_fake_poll = 1;
    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x701, 71, 0.5f, 0.5f); /* lift the last test's fingers */
    fake_finger(0x701, 5, 0.5f, 0.5f);
    fake_finger(0x701, 6, 0.5f, 0.5f);
    input_pump();
    reset_input();
    s3ePointerUpdate();
    s3ePointerUpdate();
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 0);

    g_pointer_callbacks[0].callback = (void *)(uintptr_t)&log_menu_button;
    g_pointer_callbacks[1].callback = (void *)(uintptr_t)&log_menu_motion;
    g_pointer_log_count = 0;
    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x700, 7, 0.75f, 0.25f);
    input_pump(); /* mid-frame: after this frame's read */
    assert(g_pointer_log_count == 2 && memcmp(g_pointer_log, "MD", 2) == 0);
    assert(g_pointer_log_x == 480 && g_pointer_log_y == 120);
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 0 && s3ePointerGetX() == 480); /* over it, not pressed */
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 2);
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 1);

    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x701, 7, 0.75f, 0.25f);
    input_pump();
    assert(g_pointer_log_count == 3 && g_pointer_log[2] == 'U');
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 4);
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 0);

    /* A quick tap, up before its press showed: still unpressed, pressed, then released. */
    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x700, 8, 0.25f, 0.25f);
    fake_finger(0x701, 8, 0.25f, 0.25f);
    input_pump();
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 0);
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 2);
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 4);
    s3ePointerUpdate();
    assert(s3ePointerGetState(0) == 0);

    /* Two quick taps in one frame: both presses and releases, in order. */
    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x700, 10, 0.25f, 0.25f);
    fake_finger(0x701, 10, 0.25f, 0.25f);
    fake_finger(0x700, 11, 0.5f, 0.5f);
    fake_finger(0x701, 11, 0.5f, 0.5f);
    input_pump();
    static const int32_t taps[] = {0, 2, 4, 0, 2, 4, 0};
    for (size_t i = 0; i < sizeof(taps) / sizeof(taps[0]); ++i) {
        s3ePointerUpdate();
        assert(s3ePointerGetState(0) == taps[i]);
    }
    g_pointer_callbacks[0].callback = NULL;
    g_pointer_callbacks[1].callback = NULL;
}

/* Touch screens: Marmalade calls every callback registered for an event, so the touch manager and
 * the UI both take the touch events. (The test runs in touch mode; a PC keeps the last one.) */
static int g_touch_calls_a, g_touch_calls_b;
static int32_t count_touch_a(void *system_data, void *user_data) {
    (void)system_data; (void)user_data;
    ++g_touch_calls_a;
    return 0;
}
static int32_t count_touch_b(void *system_data, void *user_data) {
    (void)system_data; (void)user_data;
    ++g_touch_calls_b;
    return 0;
}

static void test_pointer_callbacks_all_called(void) {
    reset_input();
    s3ePointerRegister(2, (void *)(uintptr_t)&count_touch_a, NULL);
    s3ePointerRegister(2, (void *)(uintptr_t)&count_touch_b, NULL);
    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x700, 9, 0.5f, 0.5f);
    fake_finger(0x701, 9, 0.5f, 0.5f);
    input_pump();
    assert(g_touch_calls_a == 2 && g_touch_calls_b == 2);
    s3ePointerUnRegister(2, (void *)(uintptr_t)&count_touch_b);
    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x700, 9, 0.5f, 0.5f);
    input_pump();
    assert(g_touch_calls_a == 3 && g_touch_calls_b == 2);
    s3ePointerUnRegister(2, (void *)(uintptr_t)&count_touch_a);
    g_fake_event_count = g_fake_event_next = 0;
    fake_finger(0x701, 9, 0.5f, 0.5f);
    input_pump();
}

int main(void) {
    setenv("BOZ_TOUCH_SCREEN", "1", 1); /* touch mode only changes desktop and touch input */
    test_skips_unmapped_joysticks();
    test_retries_controller_discovery();
    test_disconnect_releases_input_and_reconnects();
    test_first_hold_publishes_down_until_release();
    test_overlapping_shoot_sources_release_once();
    test_trigger_hysteresis_ignores_threshold_jitter();
    test_opposite_edges_survive_until_publication();
    test_cursor_mode_releases_and_reacquires_held_action();
    test_touchpad_initial_position_precedes_press();
    test_cursor_position_precedes_press();
    test_touch_screen_fingers();
    test_touch_tap_hovers_then_presses();
    test_pointer_callbacks_all_called();
    reset_input();
    input_shutdown();
    puts("s3e_input_test: ok");
    return 0;
}
