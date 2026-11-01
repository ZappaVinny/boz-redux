#ifndef BOZ_GAME_PATCHES_H
#define BOZ_GAME_PATCHES_H

/* Fixes and PC changes the client makes to BOZ 1.0.11 itself (src/game_patches/). Game features
 * belong in Lua mods; these are what a PC build needs to run well. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "s3e_image.h"

/* Smooth motion between the game's fixed simulation steps (the director's interpolation slot).
 * Needs the game image's base set (game_set_image_base) and gamedef loaded. */
bool frame_interpolation_install(void);

/* The game's known null-pointer crashes repaired in the emulator's fault hook. */
void crash_recovery_install(uint32_t image_base);

/* The account name the game builds from "Player-%d" points at player_name_address instead. */
bool player_name_install(struct s3e_loaded_image *loaded, uint32_t player_name_address);

/* The interaction prompt without its touch/Xperia X button (prompt_button.c); off with
 * BOZ_TOUCHPAD_CONTROLS. */
bool prompt_button_install(struct s3e_loaded_image *loaded);

/* The HUD's touch stick art in ingame.group.bin made transparent (hud_sticks.c). */
bool hud_sticks_apply_to(const char *name);
bool hud_sticks_hide(uint8_t *data, size_t size);

#endif
