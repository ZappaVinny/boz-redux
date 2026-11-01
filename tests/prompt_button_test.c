/* The interaction prompt's X button: hidden by patching its two SetVisible calls, left alone in
 * another game build and with touchpad controls. */
#include "game_patches.h"
#include "posix_compat.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    BUTTON = 0x19fa16,
    IMAGE = 0x19fa2c,
    SIZE = 0x19fa40,
};

static void set_show(uint8_t *memory) {
    memory[BUTTON] = 0x29, memory[BUTTON + 1] = 0x46;   /* mov r1, r5 */
    memory[IMAGE] = 0x29, memory[IMAGE + 1] = 0x46;
}

int main(void) {
    uint8_t *memory = calloc(1, SIZE);
    assert(memory);
    struct s3e_loaded_image loaded = {.base = memory, .map_size = SIZE};

    /* The game's prompt: both SetVisible(element, showButton) calls get 0. */
    set_show(memory);
    assert(prompt_button_install(&loaded));
    assert(memory[BUTTON] == 0x00 && memory[BUTTON + 1] == 0x21);   /* movs r1, #0 */
    assert(memory[IMAGE] == 0x00 && memory[IMAGE + 1] == 0x21);

    /* Another game build: left alone. */
    memset(memory, 0, SIZE);
    assert(!prompt_button_install(&loaded));
    assert(memory[BUTTON] == 0 && memory[IMAGE] == 0);

    /* Touch controls keep the button. */
    set_show(memory);
    setenv("BOZ_TOUCHPAD_CONTROLS", "1", 1);
    assert(prompt_button_install(&loaded));
    assert(memory[BUTTON] == 0x29 && memory[IMAGE] == 0x29);
    unsetenv("BOZ_TOUCHPAD_CONTROLS");

    free(memory);
    puts("prompt_button_test: ok");
    return 0;
}
