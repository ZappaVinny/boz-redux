/* Workarounds for known crashes in BOZ 1.0.11 in the emulator's fault hook. Each one matches the
 * exact faulting instruction (an image offset inside a function, so not a gamedef name) and its
 * register state, then repairs the registers so the game carries on. */
#include "game_patches.h"

#include "arm_emu.h"
#include "guest_memory.h"

#include <stdint.h>
#include <string.h>

enum {
    BUCKET_ALLOCATOR_TABLE_SLOT = 0x4c,
    BUCKET_ALLOCATOR_PREPARE = 0x374be8,
    BUCKET_ALLOCATOR_LOOKUP = 0x374bf4,
    BUCKET_ALLOCATOR_LOOKUP_DONE = 0x374c34,
    NULL_BUFFER_WRITE = 0xcb8ee,
    NULL_BUFFER_SLOT_A = 0xd291c,
    NULL_BUFFER_SLOT_B = 0xd293a,
    NULL_STRING_R1_A = 0x368ddc,
    NULL_STRING_R1_B = 0x368d2c,
    NULL_STRING_R0_A = 0x24ba00,
    NULL_STRING_R0_B = 0x24ba30,
    NULL_STRING_SKIP = 0x368fa4,
    NULL_STRING_SKIP_TARGET = 0x369024,
};

enum { R0, R1, R2, R3, R4, R5, R6, R7, SP = 13, PC = 15 };

static uint32_t g_base;
/* Data handed to the game, so in guest memory: an empty string and a bucket table. */
static char *g_empty_string;
static uint32_t *g_bucket_allocator_table;
enum { BUCKET_TABLE_WORDS = 33 };

static uint32_t host_address(const void *pointer) {
    return GUEST_ADDRESS(pointer);
}

static uint32_t *guest_words(uint32_t address) {
    return (uint32_t *)GUEST_POINTER(address);
}

static void attach_bucket_allocator_table(uint32_t object) {
    guest_words(object + BUCKET_ALLOCATOR_TABLE_SLOT)[0] = host_address(g_bucket_allocator_table);
}

static bool recover_bucket_allocator(struct arm_emu_fault *f, uint32_t offset) {
    if (offset != BUCKET_ALLOCATOR_PREPARE && offset != BUCKET_ALLOCATOR_LOOKUP) {
        return false;
    }
    uint32_t object = f->r[R5] ? f->r[R5] : f->r[R0];
    uint32_t index = f->r[R4] ? f->r[R4] : f->r[R1];
    if (!object) {
        return false;
    }
    if (index >= 32 && offset == BUCKET_ALLOCATOR_PREPARE) {
        index = 1;
    }
    if (index >= 32) {
        return false;
    }
    attach_bucket_allocator_table(object);
    f->r[R0] = object;
    f->r[R1] = index;
    f->r[R2] = host_address(g_bucket_allocator_table);
    f->r[R4] = index;
    f->r[R5] = object;
    if (offset == BUCKET_ALLOCATOR_PREPARE) {
        memset(g_bucket_allocator_table, 0, BUCKET_TABLE_WORDS * sizeof(uint32_t));
    } else {
        g_bucket_allocator_table[index] = 0;
        f->r[PC] = g_base + BUCKET_ALLOCATOR_LOOKUP_DONE;
    }
    return true;
}

/* Return early from a Thumb function: restore the registers it pushed and pop the return address. */
static void return_from_frame(struct arm_emu_fault *f, int first_register, int saved_registers) {
    const uint32_t *stack = guest_words(f->r[SP]);
    for (int i = 0; i < saved_registers; ++i) {
        f->r[first_register + i] = stack[i];
    }
    uint32_t return_address = stack[saved_registers];
    f->r[R0] = 0;
    f->r[SP] += (uint32_t)(saved_registers + 1) * 4u;
    f->r[PC] = return_address & ~1u;
    f->thumb = (return_address & 1u) != 0;
}

static bool recover(struct arm_emu_fault *f) {
    uint32_t offset = f->r[PC] - g_base;
    if (recover_bucket_allocator(f, offset)) {
        return true;
    }
    if (offset == NULL_BUFFER_WRITE && f->r[R0] == 0) {
        return_from_frame(f, R4, 3);
        return true;
    }
    if ((offset == NULL_BUFFER_SLOT_A || offset == NULL_BUFFER_SLOT_B) && f->r[R2] == 0) {
        return_from_frame(f, R3, 5);
        return true;
    }
    if ((offset == NULL_STRING_R1_A || offset == NULL_STRING_R1_B) && f->r[R1] == 0) {
        f->r[R1] = host_address(g_empty_string);
        return true;
    }
    if ((offset == NULL_STRING_R0_A || offset == NULL_STRING_R0_B) && f->r[R0] == 0) {
        f->r[R0] = host_address(g_empty_string);
        return true;
    }
    if (offset == NULL_STRING_SKIP && f->r[R1] == 0) {
        f->r[R0] = 0;
        f->r[R2] = 0;
        f->r[PC] = g_base + NULL_STRING_SKIP_TARGET;
        return true;
    }
    return false;
}

void crash_recovery_install(uint32_t image_base) {
    g_base = image_base;
    if (!g_empty_string) {
        g_empty_string = guest_calloc(1, 8);
        g_bucket_allocator_table = guest_calloc(BUCKET_TABLE_WORDS, sizeof(uint32_t));
    }
    arm_emu_set_fault_handler(recover);
}
