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
/* The address the guest calls to run host function fn: fn itself when it lies below 4 GiB (always
 * on 32-bit hosts), else a slot in the emulator's code page that calls it. */
uint32_t arm_emu_guest_fn(const void *fn);
/* The same, with fn's argument kinds (host_signatures.h) so guest-base builds can translate its
 * pointer arguments and result. */
uint32_t arm_emu_guest_fn_sig(const void *fn, const char *signature);

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

/* Calls fn before the guest instruction at address runs, on the calling thread's CPU only.
 * Emulated CPUs never patch the game's code and ignore the Thumb bit; the native CPU
 * (arm_emu_native.c) patches it and reads the mode from the Thumb bit, else from the gamedef.
 * Returns an id, or -1. */
int arm_emu_hook_add(uint32_t address, arm_emu_code_hook fn, void *user);
void arm_emu_hook_remove(int id);
/* An address in the emulator's own code page that guest code may jump to but that never runs:
 * hook it (arm_emu_hook_add) and move on from the hook, e.g. as a return trap. */
uint32_t arm_emu_trap_address(void);

/* Running totals for the performance statistics line (BOZ_PERF_STATS). Hook time is only
 * measured while the statistics are on. */
struct arm_emu_perf {
    unsigned long host_calls;
    uint64_t host_us; /* main game thread, without guest callbacks made from host code */
    unsigned long hook_calls;
    uint64_t hook_us;
    unsigned long jit_flushes;
};
void arm_emu_perf_read(struct arm_emu_perf *out);

#endif
