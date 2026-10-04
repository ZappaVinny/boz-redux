#ifndef BOZ_NATIVE_INPUT_H
#define BOZ_NATIVE_INPUT_H

/* Native PC controls: mouse look, movement and actions go straight to the game's player code
 * instead of through the emulated Xperia Play touchpads and keys. The addresses come from the
 * game definition (gamedef/); without them, or in Dead Ops Arcade, the client keeps using the
 * touchpads. Everything here runs on the game thread. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's button actions (CInputManager::OnInputAction). */
enum native_action {
    NATIVE_SHOOT,
    NATIVE_AIM,
    NATIVE_RELOAD,
    NATIVE_USE,
    NATIVE_MELEE,
    NATIVE_GRENADE,
    NATIVE_TACTICAL,
    NATIVE_CROUCH,
    NATIVE_FIRE_MODE,
    NATIVE_SWITCH_WEAPON,
    NATIVE_ACTION_COUNT,
};

enum native_match {
    NATIVE_MATCH_NONE,     /* menus, loading, or a match that is paused */
    NATIVE_MATCH_ZOMBIES,
    NATIVE_MATCH_DEAD_OPS,
};

/* Finds the game functions and installs the hooks. Call once on the game thread after the image
 * is mapped and gamedef is loaded. False when native controls are unavailable. */
bool native_input_init(void);
bool native_input_available(void);

/* Which match is running unpaused right now (its player update ran in the last moments). */
enum native_match native_input_match(void);

/* Mouse movement in counts since the last call; applied at the player's next update. */
void native_input_look(float dx, float dy);

/* The movement keys: x right, y back (forward is negative), length at most 1. Sent once per
 * frame while non-zero. */
void native_input_move(float x, float y, bool sprint);

/* Action names as Lua sees them ("shoot", "aim", ...), and back; -1 for an unknown name. */
const char *native_input_action_name(enum native_action action);
int native_input_action_from_name(const char *name);

/* A button changed. Repeated calls with the same state are ignored. Mods see it first (the Lua
 * "action" event) and may keep it from the game. */
void native_input_action(enum native_action action, bool down);
/* Sends an action to the game as is: no mod event, no repeat check (input.send). */
void native_input_send(enum native_action action, bool down);
bool native_input_action_down(enum native_action action);

/* True while the player is aiming down sights. */
bool native_input_aiming(void);

/* Lets go of everything held and drops pending look (leaving game mode). */
void native_input_release(void);

#ifdef __cplusplus
}
#endif

#endif
