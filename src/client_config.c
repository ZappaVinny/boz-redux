/* client.ini: player settings next to the game data. Each setting is translated into the BOZ_*
 * environment variable the rest of the client reads, so a variable set by the user still wins
 * over the file. A commented default file is written on first run. */
#include "client_config.h"
#include "str_util.h"

#include "posix_compat.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char DEFAULT_CONFIG_HEAD[] =
    "# BOZ Redux client settings. Environment variables (BOZ_*) override these.\n"
    "\n"
    "[display]\n"
    "# Borderless fullscreen (true) or a window (false). F11 or Alt+Enter toggles while playing.\n"
    "fullscreen = true\n"
    "# on: wait for the display refresh (smoothest); off; adaptive: tear only when a frame is late.\n"
    "vsync = on\n"
    "# auto: match the display refresh rate; a number caps the frame rate; 0: no limit.\n"
    "fps_limit = auto\n"
    "# Resolution the game renders at, scaled to the window. Higher is sharper and costs more GPU.\n"
    "resolution = 1280x720\n"
    "# fit: keep the 16:9 picture with black bars; stretch: fill the window; none: no scaling.\n"
    "scaling = fit\n"
    "# Draw the port's crosshair pointer in menus instead of the system mouse pointer. A\n"
    "# controller always gets it (it is the only pointer a controller has).\n"
    "software_cursor = false\n"
    "# Windows only. auto: ANGLE (Direct3D 11, works on almost every GPU). d3d12, vulkan and\n"
    "# software use the bundled Mesa instead; software is slow but always works.\n"
    "renderer = auto\n"
    "\n"
    "[input]\n"
    "# Mouse look speed: 0.022 degrees per mouse count at 1.0, the scale Source and Quake games\n"
    "# use, so the same number feels the same.\n"
    "look_sensitivity = 3.0\n"
    "# Multiplier while aiming down sights. The zoom is already allowed for: 1.0 turns the same\n"
    "# distance on screen per mouse movement as without aiming.\n"
    "aim_sensitivity = 1.0\n"
    "# Mouse up looks down.\n"
    "invert_y = false\n"
    "# Dead Ops Arcade still drives the game's touch stick with the mouse: its speed, and stick or\n"
    "# swipe.\n"
    "mouse_sensitivity = 12000\n"
    "look_mode = stick\n"
    "\n"
    ;

static const char KEYS_INTRO[] =
    "[keys]\n"
    "# Bindings during a match: comma-separated key names (as SDL names them, e.g. W, Left Shift,\n"
    "# Space, Escape, F) or mouse buttons Mouse1 (left), Mouse2 (middle), Mouse3 (right),\n"
    "# Mouse4, Mouse5. Leave a value empty to unbind. Controllers are mapped automatically.\n";

static const char DEFAULT_CONFIG_TAIL[] =
    "\n"
    "[player]\n"
    "# Your name in the game: letters, digits, space, - _ . (at most 13 characters).\n"
    "name = Player\n"
    "\n"
    "[network]\n"
    "# Online play: a server that answers for the game's online service (empty: offline).\n"
    "server =\n"
    "voice_chat = false\n"
    "\n"
    "[mods]\n"
    "# Set by the launcher's Mods tab. order: load order (when two mods replace the same file, the\n"
    "# later one wins); disabled: mods switched off. Mods not listed load last, switched on.\n"
    "order =\n"
    "disabled =\n"
    "\n"
    "[debug]\n"
    "# Print emulator and frame rate statistics every two seconds.\n"
    "status = false\n"
    "# Log every file the game opens and where it came from (finds the paths mods replace).\n"
    "log_files = false\n";

const struct client_binding CLIENT_BINDINGS[] = {
    {"move_forward", "Move forward", "W", NULL},
    {"move_back", "Move back", "S", NULL},
    {"move_left", "Move left", "A", NULL},
    {"move_right", "Move right", "D", NULL},
    {"shoot", "Shoot", "Mouse1", NULL},
    {"aim", "Aim", "Mouse3", NULL},
    {"reload", "Reload", "R", NULL},
    {"action", "Use", "E, F", "Use: buy, open doors, pick up, revive."},
    {"sprint", "Sprint", "Left Shift", NULL},
    {"melee", "Knife", "V", NULL},
    {"grenade", "Grenade", "G", NULL},
    {"tactical", "Tactical", "Q", NULL},
    {"crouch", "Crouch", "C, Space", NULL},
    {"alt_fire", "Alternate fire", "X", NULL},
    {"switch_weapon", "Switch weapon", "1", NULL},
    {"pause", "Pause", "Escape", NULL},
    {"toggle_mode", "Free the mouse", "Tab",
     "The mouse is captured while a match runs and freed in menus and when paused. This key frees\n"
     "# it during a match until pressed again."},
    {"fullscreen", "Toggle fullscreen", "F11", "Fullscreen toggle (Alt+Enter also works)."},
};
const int CLIENT_BINDING_COUNT = (int)(sizeof(CLIENT_BINDINGS) / sizeof(CLIENT_BINDINGS[0]));

static bool parse_bool(const char *value, bool *result) {
    if (!strcmp(value, "true") || !strcmp(value, "on") || !strcmp(value, "yes") ||
        !strcmp(value, "1")) {
        *result = true;
        return true;
    }
    if (!strcmp(value, "false") || !strcmp(value, "off") || !strcmp(value, "no") ||
        !strcmp(value, "0")) {
        *result = false;
        return true;
    }
    return false;
}

static bool user_set(const char *variable) {
    return getenv(variable) != NULL;
}

static void set_default(const char *variable, const char *value) {
    if (!user_set(variable)) {
        setenv(variable, value, 1);
    }
}

static bool is_number(const char *value) {
    if (!*value) {
        return false;
    }
    for (const char *c = value; *c; ++c) {
        if (!isdigit((unsigned char)*c) && *c != '.') {
            return false;
        }
    }
    return true;
}

enum { MAX_BINDINGS = 32 };

static struct {
    char action[32];
    char value[96];
} g_bindings[MAX_BINDINGS];
static int g_binding_count;

static char g_mods_order[1024];
static char g_mods_disabled[1024];

const char *client_config_mods_order(void) {
    return g_mods_order;
}

const char *client_config_mods_disabled(void) {
    return g_mods_disabled;
}

static void store_binding(const char *action, const char *value) {
    int slot = 0;
    while (slot < g_binding_count && strcmp(g_bindings[slot].action, action) != 0) {
        ++slot;
    }
    if (slot == g_binding_count) {
        if (g_binding_count == MAX_BINDINGS) {
            return;
        }
        ++g_binding_count;
    }
    snprintf(g_bindings[slot].action, sizeof(g_bindings[slot].action), "%s", action);
    snprintf(g_bindings[slot].value, sizeof(g_bindings[slot].value), "%s", value);
}

const char *client_config_key_binding(const char *action) {
    for (int i = 0; i < g_binding_count; ++i) {
        if (!strcmp(g_bindings[i].action, action)) {
            return g_bindings[i].value;
        }
    }
    return NULL;
}

/* Returns false for an unknown key or a bad value. [keys] values are checked by the input code. */
static bool apply(const char *section, const char *key, const char *value) {
    bool flag;
    if (!strcmp(section, "keys")) {
        store_binding(key, value);
        return true;
    }
    if (!strcmp(section, "mods")) {
        if (!strcmp(key, "order")) {
            snprintf(g_mods_order, sizeof(g_mods_order), "%s", value);
            return true;
        }
        if (!strcmp(key, "disabled")) {
            snprintf(g_mods_disabled, sizeof(g_mods_disabled), "%s", value);
            return true;
        }
        return false;
    }
    if (!strcmp(section, "player")) {
        if (!strcmp(key, "name")) {
            set_default("BOZ_PLAYER_NAME", value);
            return true;
        }
        return false;
    }
    if (!strcmp(section, "network")) {
        if (!strcmp(key, "server")) {
            set_default("BOZ_MULTIPLAYER_SERVER", value);
            return true;
        }
        if (!strcmp(key, "voice_chat") && parse_bool(value, &flag)) {
            set_default("BOZ_VOICE_CHAT", flag ? "1" : "0");
            return true;
        }
        return false;
    }
    if (!strcmp(section, "display")) {
        if (!strcmp(key, "fullscreen") && parse_bool(value, &flag)) {
            set_default("BOZ_WINDOWED", flag ? "0" : "1");
            return true;
        }
        if (!strcmp(key, "vsync")) {
            const char *interval = !strcmp(value, "adaptive")      ? "-1"
                                   : parse_bool(value, &flag) ? (flag ? "1" : "0")
                                                              : NULL;
            if (interval) {
                set_default("BOZ_VSYNC", interval);
            }
            return interval != NULL;
        }
        if (!strcmp(key, "fps_limit")) {
            if (!strcmp(value, "auto")) {
                return true;
            }
            if (is_number(value)) {
                set_default("BOZ_FPS", value);
                return true;
            }
            return false;
        }
        if (!strcmp(key, "resolution")) {
            unsigned width = 0, height = 0;
            char extra;
            if (sscanf(value, "%ux%u%c", &width, &height, &extra) != 2 || !width || !height) {
                return false;
            }
            set_default("BOZ_DISPLAY", value);
            return true;
        }
        if (!strcmp(key, "renderer")) {
            /* Windows: ANGLE (Direct3D 11) by default; the others use the bundled Mesa. */
            const char *mesa_driver = !strcmp(value, "d3d12") || !strcmp(value, "gpu") ? "d3d12"
                                      : !strcmp(value, "vulkan")                       ? "zink"
                                      : !strcmp(value, "software")                     ? "llvmpipe"
                                                                                       : NULL;
            if (mesa_driver) {
                set_default("BOZ_RENDERER", "mesa");
                set_default("GALLIUM_DRIVER", mesa_driver);
            } else if (strcmp(value, "auto") != 0 && strcmp(value, "angle") != 0) {
                return false;
            }
            return true;
        }
        if (!strcmp(key, "software_cursor") && parse_bool(value, &flag)) {
            if (flag) {
                set_default("BOZ_SOFTWARE_CURSOR", "1");
            }
            return true;
        }
        if (!strcmp(key, "scaling")) {
            if (user_set("BOZ_STRETCH") || user_set("BOZ_NO_SCALE")) {
                return true;
            }
            if (!strcmp(value, "stretch")) {
                setenv("BOZ_STRETCH", "1", 1);
            } else if (!strcmp(value, "none")) {
                setenv("BOZ_NO_SCALE", "1", 1);
            } else if (strcmp(value, "fit") != 0) {
                return false;
            }
            return true;
        }
    } else if (!strcmp(section, "input")) {
        if (!strcmp(key, "look_sensitivity") && is_number(value)) {
            set_default("BOZ_LOOK_SENS", value);
            return true;
        }
        if (!strcmp(key, "aim_sensitivity") && is_number(value)) {
            set_default("BOZ_AIM_SENS", value);
            return true;
        }
        if (!strcmp(key, "invert_y") && parse_bool(value, &flag)) {
            if (flag) {
                set_default("BOZ_INVERT_Y", "1");
            }
            return true;
        }
        if (!strcmp(key, "mouse_sensitivity") && is_number(value)) {
            set_default("BOZ_MOUSE_SENS", value);
            return true;
        }
        if (!strcmp(key, "look_mode") && (!strcmp(value, "stick") || !strcmp(value, "swipe"))) {
            set_default("BOZ_LOOK_MODE", value);
            return true;
        }
        if (!strcmp(key, "look_radius") && (is_number(value) || !*value)) {
            if (*value) {
                set_default("BOZ_LOOK_RADIUS", value);
            }
            return true;
        }
    } else if (!strcmp(section, "debug")) {
        if (!strcmp(key, "status") && parse_bool(value, &flag)) {
            if (flag) {
                set_default("BOZ_TRACE_STATUS", "1");
            }
            return true;
        }
        if (!strcmp(key, "log_files") && parse_bool(value, &flag)) {
            if (flag) {
                set_default("BOZ_TRACE_FILES", "1");
            }
            return true;
        }
    }
    return false;
}

bool client_config_write_default(const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "[config] cannot create %s\n", path);
        return false;
    }
    fputs(DEFAULT_CONFIG_HEAD, file);
    fputs(KEYS_INTRO, file);
    for (int i = 0; i < CLIENT_BINDING_COUNT; ++i) {
        if (CLIENT_BINDINGS[i].comment) {
            fprintf(file, "# %s\n", CLIENT_BINDINGS[i].comment);
        }
        fprintf(file, "%s = %s\n", CLIENT_BINDINGS[i].action, CLIENT_BINDINGS[i].defaults);
    }
    fputs(DEFAULT_CONFIG_TAIL, file);
    fclose(file);
    fprintf(stderr, "[config] wrote default settings to %s\n", path);
    return true;
}

void client_config_load(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/client.ini", root && root[0] ? root : ".");
    FILE *file = fopen(path, "r");
    if (!file) {
        client_config_write_default(path);
        file = fopen(path, "r");
        if (!file) {
            return;
        }
    }
    char line[512];
    char section[64] = "";
    int number = 0;
    while (fgets(line, sizeof(line), file)) {
        ++number;
        char *text = str_trim(line);
        if (!*text || *text == '#' || *text == ';') {
            continue;
        }
        if (*text == '[') {
            char *close = strchr(text, ']');
            if (close) {
                *close = '\0';
                snprintf(section, sizeof(section), "%s", str_trim(text + 1));
                continue;
            }
        }
        char *equals = strchr(text, '=');
        if (!equals) {
            fprintf(stderr, "[config] %s:%d: expected key = value\n", path, number);
            continue;
        }
        *equals = '\0';
        char *key = str_trim(text);
        char *value = str_trim(equals + 1);
        char *comment = strpbrk(value, "#;");
        if (comment) {
            *comment = '\0';
            value = str_trim(value);
        }
        if (!apply(section, key, value)) {
            fprintf(stderr, "[config] %s:%d: ignoring [%s] %s = %s\n", path, number, section, key,
                    value);
        }
    }
    fclose(file);
}
