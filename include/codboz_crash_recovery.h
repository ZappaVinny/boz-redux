#ifndef CODBOZ_CRASH_RECOVERY_H
#define CODBOZ_CRASH_RECOVERY_H

#include <stdint.h>

/* Emulator builds: repair the game's known null-pointer crashes instead of stopping. */
void codboz_install_crash_recovery(uint32_t image_base);

#endif
