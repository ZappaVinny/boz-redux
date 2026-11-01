#include "game_patches.h"

#include <stdlib.h>
#include <string.h>

/* Poi_ShowPrompt(element, text, show, showButton) shows the interaction prompt's "Button" and
 * "Image" elements (the touch/Xperia X button) with SetVisible(element, showButton). On PC the
 * prompt's text is enough: both calls get 0 instead (mov r1,r5 -> movs r1,#0). The use-button
 * flag the same function sets from showButton stays as it is, so using things still works. */
enum {
    PROMPT_BUTTON_VISIBLE_OFFSET = 0x19fa16,
    PROMPT_IMAGE_VISIBLE_OFFSET = 0x19fa2c,
};

static const uint8_t SHOW_BUTTON[2] = {0x29, 0x46};   /* mov r1, r5 */
static const uint8_t HIDE_BUTTON[2] = {0x00, 0x21};   /* movs r1, #0 */

bool prompt_button_install(struct s3e_loaded_image *loaded) {
    if (getenv("BOZ_TOUCHPAD_CONTROLS")) {
        return true;   /* touch play keeps the button to press */
    }
    if (!loaded || !loaded->base || loaded->map_size < PROMPT_IMAGE_VISIBLE_OFFSET + 2) {
        return false;
    }
    uint8_t *button = loaded->base + PROMPT_BUTTON_VISIBLE_OFFSET;
    uint8_t *image = loaded->base + PROMPT_IMAGE_VISIBLE_OFFSET;
    if (memcmp(button, SHOW_BUTTON, 2) != 0 || memcmp(image, SHOW_BUTTON, 2) != 0) {
        return false;
    }
    memcpy(button, HIDE_BUTTON, 2);
    memcpy(image, HIDE_BUTTON, 2);
    return true;
}
