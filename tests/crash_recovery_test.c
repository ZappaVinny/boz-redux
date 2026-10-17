/* Feeds faults to the crash-recovery handler with the guest stack in host memory (the loader is
 * 32-bit, so host pointers are guest addresses). */
#include "arm_emu.h"
#include "game_patches.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { R0, R1, R2, R3, R4, R5, R6, R7, SP = 13, PC = 15 };
enum { BASE = 0x4a000000 };

static arm_emu_fault_handler g_handler;

void arm_emu_set_fault_handler(arm_emu_fault_handler handler) {
    g_handler = handler;
}

static struct arm_emu_fault fault_at(uint32_t offset) {
    struct arm_emu_fault f;
    memset(&f, 0, sizeof(f));
    f.r[PC] = BASE + offset;
    return f;
}

static void test_return_from_frame(void) {
    /* 0xcb8ee pushed r4-r6 and lr: the handler restores them and returns 0 to the caller. */
    uint32_t stack[5] = {0x44, 0x55, 0x66, 0x4a001235, 0xdead};
    struct arm_emu_fault f = fault_at(0xcb8ee);
    f.r[SP] = (uint32_t)(uintptr_t)stack;
    f.r[R0] = 0;
    f.r[R4] = 1;
    assert(g_handler(&f));
    assert(f.r[R4] == 0x44 && f.r[R5] == 0x55 && f.r[R6] == 0x66);
    assert(f.r[R0] == 0);
    assert(f.r[SP] == (uint32_t)(uintptr_t)&stack[4]);
    assert(f.r[PC] == 0x4a001234 && f.thumb);

    /* 0xd291c pushed r3-r7 and lr; an ARM return address leaves Thumb off. */
    uint32_t wide[7] = {3, 4, 5, 6, 7, 0x4a002000, 0};
    f = fault_at(0xd291c);
    f.r[SP] = (uint32_t)(uintptr_t)wide;
    f.thumb = true;
    assert(g_handler(&f));
    assert(f.r[R3] == 3 && f.r[R7] == 7);
    assert(f.r[SP] == (uint32_t)(uintptr_t)&wide[6]);
    assert(f.r[PC] == 0x4a002000 && !f.thumb);
}

static void test_register_conditions(void) {
    struct arm_emu_fault f = fault_at(0xcb8ee);
    f.r[R0] = 1;
    assert(!g_handler(&f));

    f = fault_at(0x368ddc);
    assert(g_handler(&f));
    assert(f.r[R1] != 0 && *(const char *)(uintptr_t)f.r[R1] == '\0');

    f = fault_at(0x368fa4);
    assert(g_handler(&f));
    assert(f.r[PC] == BASE + 0x369024);

    f = fault_at(0x123456);
    assert(!g_handler(&f));
}

int main(void) {
    crash_recovery_install(BASE);
    assert(g_handler);
    test_return_from_frame();
    test_register_conditions();
    puts("crash recovery tests passed");
    return 0;
}
