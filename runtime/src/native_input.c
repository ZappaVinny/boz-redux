/* Native PC controls (see native_input.h). How the game handles input, top-down:
 *
 * - The touch pads and buttons call CInputManager: buttons go through OnInputAction(id), the two
 *   analogue pads through SendAnalogStick(angle, magnitude, radius fraction, x, y, is_look), which
 *   sends ON_ANALOG_STICK to CPlayerController::OnEvent.
 * - OnEvent stores the frame's stick input on the controller; CPlayerController::FixedStep moves
 *   and turns the player from it; CPlayerController::LateUpdate clears it.
 * - Turning multiplies the look stick by its own magnitude, TurnSpeed and a sensitivity curve,
 *   which is what made mouse look feel like a stick.
 *
 * So the client sends the movement keys as the move pad would (one SendAnalogStick per frame,
 * from the move pad's own update), the buttons as OnInputAction, and adds mouse movement straight
 * to the controller's yaw and pitch at the start of FixedStep. The game still clamps the pitch,
 * applies recoil and rebuilds the view. */
#include "native_input.h"

#include "arm_emu.h"
#include "mod_runtime.h"
#include "s3e_host_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CPlayerController fields (gamedef notes on CPlayerController::FixedStep and OnEvent). */
enum {
    PC_WEAPON_MANAGER = 0x88,
    PC_LOOK_ACTIVE = 0x154,  /* u8: look input this frame; the view is rebuilt only when set */
    PC_LOOK_ALLOWED = 0x15b, /* u8 */
    PC_YAW = 0x170,          /* float, radians */
    PC_PITCH = 0x174,        /* float, radians, clamped to +-MaxPitch */
    PC_STATE = 0x1cc + 8,    /* CIsStateMachine current state id; 8 = an aiming movement state */
    PC_STATE_AIMING = 8,
    /* CWeaponManager */
    WM_AIMING = 0xbc,        /* u8: set by its aim setter while zoomed in */
    WM_CURRENT_FOV = 0xc0,   /* float, degrees */
    WM_BASE_FOV = 0xc4,
    WM_RECOIL_ACTIVE = 0xfa, /* u8: turning counts against recoil (+0x104/+0x108) */
    WM_TURN_YAW = 0x104,
    WM_TURN_PITCH = 0x108,
    /* CTouchComponentAnaloguePad */
    PAD_INDEX = 0x7c,        /* 0 move, 1 look */
    /* CInputManager */
    IM_SUBJECT = 0x20,       /* CIsSubject the input events go out on */
    IM_RELOAD = 0x43,        /* u8: reload pressed this frame */
    IM_AUTO_FIRE = 0x4c,     /* u8: keeps the fire button held on automatic weapons */
    CVAR_VALUE = 0x10,
};

/* OnInputAction ids for press and release; 0 when the game has no release action. Reload is
 * not one: action 4/5 is the touch weapon button (tap reloads after a delay, double tap or hold
 * swaps weapons), so the client sends the reload event itself. */
static const struct {
    uint32_t press;
    uint32_t release;
} ACTION_IDS[NATIVE_ACTION_COUNT] = {
    [NATIVE_SHOOT] = {9, 10},
    [NATIVE_AIM] = {8, 0},  /* a toggle in the game */
    [NATIVE_RELOAD] = {0, 0},
    [NATIVE_USE] = {2, 3},
    [NATIVE_MELEE] = {1, 0},
    [NATIVE_GRENADE] = {6, 7},
    [NATIVE_TACTICAL] = {0xb, 0xc},
    [NATIVE_CROUCH] = {0xd, 0xe},  /* tap crouches, hold goes prone */
    [NATIVE_FIRE_MODE] = {0xf, 0},
    [NATIVE_SWITCH_WEAPON] = {0x10, 0},
};

static const char *const ACTION_NAMES[NATIVE_ACTION_COUNT] = {
    [NATIVE_SHOOT] = "shoot",       [NATIVE_AIM] = "aim",
    [NATIVE_RELOAD] = "reload",     [NATIVE_USE] = "use",
    [NATIVE_MELEE] = "melee",       [NATIVE_GRENADE] = "grenade",
    [NATIVE_TACTICAL] = "tactical", [NATIVE_CROUCH] = "crouch",
    [NATIVE_FIRE_MODE] = "fire_mode", [NATIVE_SWITCH_WEAPON] = "switch_weapon",
};

/* A match counts as running while its player update ran this recently. */
enum { MATCH_TIMEOUT_MS = 250 };

static const float PI = 3.14159265f;
/* Degrees per mouse count at sensitivity 1, as in Source and Quake games. */
static const float DEGREES_PER_COUNT = 0.022f;
/* Radius fraction the move pad reports: below sprintUp (1.5) walks, above sprints. */
static const float WALK_FRACTION = 1.0f;
static const float SPRINT_FRACTION = 3.5f;

static bool g_available;
static uint32_t g_input_manager;     /* address of CInputManager::s_instance */
static uint32_t g_on_input_action;
static uint32_t g_send_analog_stick;
static uint32_t g_console_global;
static uint32_t g_console_find_var;
static uint32_t g_notify;
static uint32_t g_hash_string;

static uint64_t g_zombies_ms;
static uint64_t g_dead_ops_ms;
static uint32_t g_controller;        /* the local player's controller, seen in FixedStep */

static float g_look_dx, g_look_dy;
static float g_sensitivity = 3.0f;
static float g_aim_sensitivity = 1.0f;
static bool g_invert_y;

static float g_move_x, g_move_y;
static bool g_move_sprint;
static bool g_move_sent;             /* the last move event was non-zero */

static bool g_down[NATIVE_ACTION_COUNT];

static uint32_t read_u32(uint32_t address) {
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static float read_float(uint32_t address) {
    float value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static void write_float(uint32_t address, float value) {
    memcpy((void *)(uintptr_t)address, &value, sizeof(value));
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float env_float(const char *name, float fallback) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = NULL;
    float parsed = strtof(value, &end);
    return end != value ? parsed : fallback;
}

static uint32_t input_manager(void) {
    return g_input_manager ? read_u32(g_input_manager) : 0;
}

/* The MaxPitch console variable, registered by the game's first FixedStep. */
static float max_pitch(void) {
    static uint32_t var;
    if (!var && g_console_global && g_console_find_var && read_u32(g_console_global)) {
        var = s3e_guest_call((const void *)(uintptr_t)g_console_find_var, 2,
                             read_u32(g_console_global), S3E_GUEST_ARG("MaxPitch"));
    }
    return var ? read_float(var + CVAR_VALUE) : 0.5f;
}

/* Turns by the pending mouse movement. Runs at the start of CPlayerController::FixedStep. */
static void on_player_fixed_step(struct arm_emu_regs *regs, void *user) {
    (void)user;
    uint32_t controller = regs->r[0];
    g_controller = controller;
    g_zombies_ms = monotonic_ms();
    if (!g_look_dx && !g_look_dy) {
        return;
    }
    float dx = g_look_dx, dy = g_look_dy;
    g_look_dx = g_look_dy = 0;
    if (!*(const uint8_t *)(uintptr_t)(controller + PC_LOOK_ALLOWED)) {
        return;
    }

    float scale = g_sensitivity * DEGREES_PER_COUNT * PI / 180.0f;
    uint32_t weapons = read_u32(controller + PC_WEAPON_MANAGER);
    if (weapons) {
        /* Keep the same feel at any zoom: scale by how much the view is narrowed. */
        float current = read_float(weapons + WM_CURRENT_FOV);
        float base = read_float(weapons + WM_BASE_FOV);
        if (current > 0 && base > current && base < 179.0f) {
            scale *= tanf(current * PI / 360.0f) / tanf(base * PI / 360.0f);
        }
    }
    if (weapons && *(const uint8_t *)(uintptr_t)(weapons + WM_AIMING)) {
        scale *= g_aim_sensitivity;
    }

    float yaw = read_float(controller + PC_YAW);
    float pitch = read_float(controller + PC_PITCH);
    float yaw_delta = -dx * scale;
    float limit = max_pitch();
    float new_pitch = pitch + (g_invert_y ? -dy : dy) * scale;
    if (new_pitch > limit) {
        new_pitch = limit;
    } else if (new_pitch < -limit) {
        new_pitch = -limit;
    }
    float new_yaw = fmodf(yaw + yaw_delta, 2.0f * PI);
    if (new_yaw < 0) {
        new_yaw += 2.0f * PI;
    }
    write_float(controller + PC_YAW, new_yaw);
    write_float(controller + PC_PITCH, new_pitch);
    *(uint8_t *)(uintptr_t)(controller + PC_LOOK_ACTIVE) = 1;
    if (weapons && *(const uint8_t *)(uintptr_t)(weapons + WM_RECOIL_ACTIVE)) {
        write_float(weapons + WM_TURN_YAW, read_float(weapons + WM_TURN_YAW) + yaw_delta);
        write_float(weapons + WM_TURN_PITCH,
                    read_float(weapons + WM_TURN_PITCH) + (new_pitch - pitch));
    }
}

static void send_analog_stick(float angle, float magnitude, float fraction, float x, float y) {
    uint32_t input = input_manager();
    if (!input) {
        return;
    }
    const uint32_t args[7] = {input, float_bits(angle), float_bits(magnitude), float_bits(fraction),
                              float_bits(x), float_bits(y), 0};
    arm_emu_call(g_send_analog_stick, 7, args);
}

/* Sends the movement keys as the move pad would. Runs at the start of the pads' update. */
static void on_pad_update(struct arm_emu_regs *regs, void *user) {
    (void)user;
    if (read_u32(regs->r[0] + PAD_INDEX) != 0) {
        return;
    }
    float length = sqrtf(g_move_x * g_move_x + g_move_y * g_move_y);
    if (length < 0.001f) {
        if (g_move_sent) {
            send_analog_stick(0, 0, 0, 0, 0);  /* what the pad sends when the finger lifts */
            g_move_sent = false;
        }
        return;
    }
    float x = g_move_x / length, y = g_move_y / length;
    float magnitude = length > 1.0f ? 1.0f : length;
    send_analog_stick(atan2f(y, x), magnitude, g_move_sprint ? SPRINT_FRACTION : WALK_FRACTION, x,
                      y);
    g_move_sent = true;
}

static void on_dead_ops_fixed_step(struct arm_emu_regs *regs, void *user) {
    (void)regs;
    (void)user;
    g_dead_ops_ms = monotonic_ms();
}

static uint32_t require(const char *name, bool *ok) {
    uint32_t address = game_symbol_address(name);
    if (!address) {
        fprintf(stderr, "[input] native controls: %s is not in the game definition\n", name);
        *ok = false;
    }
    return address;
}

static bool hook(uint32_t address, arm_emu_code_hook fn) {
    return address && arm_emu_hook_add(address & ~1u, fn, NULL) >= 0;
}

bool native_input_init(void) {
#if defined(__arm__)
    return false;
#else
    static bool started;
    if (started) {
        return g_available;
    }
    started = true;
    if (getenv("BOZ_TOUCHPAD_CONTROLS")) {
        fprintf(stderr, "[input] native controls off (BOZ_TOUCHPAD_CONTROLS)\n");
        return false;
    }
    bool ok = true;
    g_input_manager = require("CInputManager::s_instance", &ok);
    g_on_input_action = require("CInputManager::OnInputAction", &ok);
    g_send_analog_stick = require("CInputManager::SendAnalogStick", &ok);
    g_notify = require("CIsSubject::NotifyVirtual", &ok);
    g_hash_string = require("IwHashString", &ok);
    uint32_t player_step = require("CPlayerController::FixedStep", &ok);
    uint32_t pad_update = require("CTouchComponentAnaloguePad::Update", &ok);
    /* The per-player updates, not their component tables: the director runs the tables in the
     * front end too, with no players in them. */
    uint32_t dead_ops_step = game_symbol_address("CDOPlayerController::FixedStep");
    g_console_global = game_symbol_address("g_console");
    g_console_find_var = game_symbol_address("Console_FindVar");
    if (!ok) {
        return false;
    }
    if (!hook(player_step, on_player_fixed_step) || !hook(pad_update, on_pad_update)) {
        fprintf(stderr, "[input] native controls: cannot hook the player update\n");
        return false;
    }
    hook(dead_ops_step, on_dead_ops_fixed_step);
    g_sensitivity = env_float("BOZ_LOOK_SENS", 3.0f);
    g_aim_sensitivity = env_float("BOZ_AIM_SENS", 1.0f);
    const char *invert = getenv("BOZ_INVERT_Y");
    g_invert_y = invert && invert[0] && strcmp(invert, "0") != 0;
    g_available = true;
    fprintf(stderr, "[input] native controls on (sensitivity %.2f, aim %.2f%s)\n", g_sensitivity,
            g_aim_sensitivity, g_invert_y ? ", inverted" : "");
    return true;
#endif
}

bool native_input_available(void) {
    return g_available;
}

enum native_match native_input_match(void) {
    if (!g_available) {
        return NATIVE_MATCH_NONE;
    }
    uint64_t now = monotonic_ms();
    if (g_zombies_ms && now - g_zombies_ms < MATCH_TIMEOUT_MS) {
        return NATIVE_MATCH_ZOMBIES;
    }
    if (g_dead_ops_ms && now - g_dead_ops_ms < MATCH_TIMEOUT_MS) {
        return NATIVE_MATCH_DEAD_OPS;
    }
    return NATIVE_MATCH_NONE;
}

void native_input_look(float dx, float dy) {
    g_look_dx += dx;
    g_look_dy += dy;
}

void native_input_move(float x, float y, bool sprint) {
    g_move_x = x;
    g_move_y = y;
    g_move_sprint = sprint;
}

static uint32_t hash_string(const char *text) {
    return s3e_guest_call((const void *)(uintptr_t)g_hash_string, 1, S3E_GUEST_ARG(text));
}

/* What the weapon button's reload branch does: flag it and send ON_RELOAD_BUTTON. */
static void press_reload(uint32_t input) {
    static uint32_t event;
    if (!event) {
        event = hash_string("ON_RELOAD_BUTTON");
    }
    *(uint8_t *)(uintptr_t)(input + IM_RELOAD) = 1;
    s3e_guest_call((const void *)(uintptr_t)g_notify, 5, input + IM_SUBJECT, event, 1u, 0u, 0u);
}

const char *native_input_action_name(enum native_action action) {
    return action >= 0 && action < NATIVE_ACTION_COUNT ? ACTION_NAMES[action] : NULL;
}

int native_input_action_from_name(const char *name) {
    for (int i = 0; name && i < NATIVE_ACTION_COUNT; ++i) {
        if (!strcmp(ACTION_NAMES[i], name)) {
            return i;
        }
    }
    return -1;
}

void native_input_send(enum native_action action, bool down) {
    uint32_t input = input_manager();
    if (!g_available || action < 0 || action >= NATIVE_ACTION_COUNT || !input) {
        return;
    }
    if (action == NATIVE_RELOAD) {
        if (down) {
            press_reload(input);
        }
        return;
    }
    uint32_t id = down ? ACTION_IDS[action].press : ACTION_IDS[action].release;
    if (id) {
        s3e_guest_call((const void *)(uintptr_t)g_on_input_action, 3, input, id, 0u);
    }
    if (action == NATIVE_SHOOT && down) {
        /* A touch fire button only auto-fires after a double tap; a held key or button always
         * should. The game still fires single shots for semi-automatic weapons. */
        *(uint8_t *)(uintptr_t)(input + IM_AUTO_FIRE) = 1;
    }
}

void native_input_action(enum native_action action, bool down) {
    if (!g_available || action < 0 || action >= NATIVE_ACTION_COUNT || g_down[action] == down) {
        return;
    }
    g_down[action] = down;
    if (lua_runtime_action(ACTION_NAMES[action], down)) {
        return;
    }
    native_input_send(action, down);
}

bool native_input_action_down(enum native_action action) {
    return action >= 0 && action < NATIVE_ACTION_COUNT && g_down[action];
}

bool native_input_aiming(void) {
    if (!g_controller || native_input_match() != NATIVE_MATCH_ZOMBIES) {
        return false;
    }
    /* Either signal: the player's aiming state (8) covers only some of aiming, the weapon
     * manager's flag the zoom. Neither is fully reliable yet (hold-to-aim work, SDK notes). */
    uint32_t weapons = read_u32(g_controller + PC_WEAPON_MANAGER);
    int state = read_u32(g_controller + PC_STATE) == PC_STATE_AIMING;
    int flag = weapons && *(const uint8_t *)(uintptr_t)(weapons + WM_AIMING);
    return state || flag;
}

void native_input_release(void) {
    for (int i = 0; i < NATIVE_ACTION_COUNT; ++i) {
        native_input_action((enum native_action)i, false);
    }
    g_move_x = g_move_y = 0;
    g_move_sprint = false;
    g_look_dx = g_look_dy = 0;
}
