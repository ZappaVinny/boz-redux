#ifndef ARM_EMU_H
#define ARM_EMU_H

#include <stdbool.h>
#include <stdint.h>

bool arm_emu_init(void);
void arm_emu_set_image(uint32_t start, uint32_t end);
void arm_emu_register_code(uint32_t start, uint32_t size);
void arm_emu_trace_ignore(uint32_t fn);
uint64_t arm_emu_call(uint32_t fn, int argc, const uint32_t *argv);
void arm_emu_thread_exit(void);
uint32_t arm_emu_call_scratch(void);
uint64_t arm_emu_call_host(uint32_t fn, const uint32_t *args);

/* A guest memory fault. r[15] is the faulting instruction (no Thumb bit); thumb is its mode. */
struct arm_emu_fault {
    uint32_t r[16];
    uint32_t address;
    bool write;
    bool thumb;
};

/* Called when guest code touches unmapped memory. Return true after editing r[] (and thumb) to
 * resume at r[15]; false lets the fault stop the game. */
typedef bool (*arm_emu_fault_handler)(struct arm_emu_fault *fault);
void arm_emu_set_fault_handler(arm_emu_fault_handler handler);

/* Guest registers at a code hook. r[15] is the hooked address (no Thumb bit). Edit r[0..14] to
 * change them; set r[15] (and thumb) to continue somewhere else, e.g. at r[14] to return early. */
struct arm_emu_regs {
    uint32_t r[16];
    bool thumb;
};

typedef void (*arm_emu_code_hook)(struct arm_emu_regs *regs, void *user);

/* Calls fn before the guest instruction at address runs (Thumb bit ignored), on the calling
 * thread's CPU only; the game's code is never patched. Returns an id, or -1. */
int arm_emu_hook_add(uint32_t address, arm_emu_code_hook fn, void *user);
void arm_emu_hook_remove(int id);
/* An address in the emulator's own code page that guest code may jump to but that never runs:
 * hook it (arm_emu_hook_add) and move on from the hook, e.g. as a return trap. */
uint32_t arm_emu_trap_address(void);

#endif
