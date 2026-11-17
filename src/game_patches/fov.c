/* The field of view the launcher sets (client.ini [display] fov, BOZ_FOV): the game's own number
 * for the view when not aiming down sights (gamedef CWeaponManager.baseFov; 50 in most maps).
 * CWeaponManager::UpdateFov eases the view towards it every frame, and each match starts with
 * the map's value: the hook puts ours in place of that value. A value a mod sets (boz.camera,
 * the Developer console's fov) is left alone; aiming still zooms. */
#include "game_patches.h"

#include "arm_emu.h"
#include "client_config.h"
#include "mod_runtime.h"
#include "guest_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BASE_FOV = 0xc4 };   /* CWeaponManager.baseFov (gamedef) */

static float g_fov;
static uint32_t g_manager;
static float g_game_value;   /* the map's value the game set for this manager */

static void on_update_fov(struct arm_emu_regs *regs, void *user) {
    (void)user;
    uint32_t manager = regs->r[0];
    if (!manager) {
        return;
    }
    float *base = (float *)GUEST_POINTER(manager + BASE_FOV);
    if (manager != g_manager) {
        g_manager = manager;
        g_game_value = *base;
        *base = g_fov;
    } else if (*base == g_game_value) {   /* the game set the map's value again */
        *base = g_fov;
    }
}

bool fov_install(void) {
    const char *setting = getenv("BOZ_FOV");
    if (!setting || !setting[0] || !strcmp(setting, "0")) {
        return true;   /* the map's own */
    }
    if (!fov_parse(setting, &g_fov)) {
        fprintf(stderr, "[fov] ignoring field of view %s (%d-%d)\n", setting, FOV_MIN, FOV_MAX);
        return false;
    }
    uint32_t update = game_symbol_address("CWeaponManager::UpdateFov");
    if (!update || arm_emu_hook_add(update & ~1u, on_update_fov, NULL) < 0) {
        fprintf(stderr, "[fov] cannot hook CWeaponManager::UpdateFov\n");
        return false;
    }
    fprintf(stderr, "[fov] field of view %.0f\n", g_fov);
    return true;
}
