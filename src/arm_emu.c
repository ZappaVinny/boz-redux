#include "arm_emu.h"
#include "platform/platform.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unicorn/unicorn.h>

enum {
    PAGE_SIZE_EMU = 0x1000,
    STACK_SIZE = 2 * 1024 * 1024,
    HOST_ARG_WORDS = 16,
    SVC_ARM_WORD = 0xef00df00u,
};

struct arm_emu {
    uc_engine *uc;
    uint8_t *stack;
    uint32_t sentinel;
    int depth;
};

#if defined(__i386__)
/* Calls fn with 16 stack words and restores esp afterwards, so cdecl and stdcall callees
   (Windows GL/EGL entry points pop their own arguments) both return a balanced stack. */
static uint64_t call_host16(uint32_t fn, const uint32_t *args) {
    uint32_t lo, hi;
    __asm__ volatile("movl %%esp, %%esi\n\t"
                     "andl $-16, %%esp\n\t"
                     "subl $64, %%esp\n\t"
                     "movl 0(%%edi), %%eax\n\t movl %%eax, 0(%%esp)\n\t"
                     "movl 4(%%edi), %%eax\n\t movl %%eax, 4(%%esp)\n\t"
                     "movl 8(%%edi), %%eax\n\t movl %%eax, 8(%%esp)\n\t"
                     "movl 12(%%edi), %%eax\n\t movl %%eax, 12(%%esp)\n\t"
                     "movl 16(%%edi), %%eax\n\t movl %%eax, 16(%%esp)\n\t"
                     "movl 20(%%edi), %%eax\n\t movl %%eax, 20(%%esp)\n\t"
                     "movl 24(%%edi), %%eax\n\t movl %%eax, 24(%%esp)\n\t"
                     "movl 28(%%edi), %%eax\n\t movl %%eax, 28(%%esp)\n\t"
                     "movl 32(%%edi), %%eax\n\t movl %%eax, 32(%%esp)\n\t"
                     "movl 36(%%edi), %%eax\n\t movl %%eax, 36(%%esp)\n\t"
                     "movl 40(%%edi), %%eax\n\t movl %%eax, 40(%%esp)\n\t"
                     "movl 44(%%edi), %%eax\n\t movl %%eax, 44(%%esp)\n\t"
                     "movl 48(%%edi), %%eax\n\t movl %%eax, 48(%%esp)\n\t"
                     "movl 52(%%edi), %%eax\n\t movl %%eax, 52(%%esp)\n\t"
                     "movl 56(%%edi), %%eax\n\t movl %%eax, 56(%%esp)\n\t"
                     "movl 60(%%edi), %%eax\n\t movl %%eax, 60(%%esp)\n\t"
                     "call *%%ecx\n\t"
                     "movl %%esi, %%esp\n\t"
                     : "=a"(lo), "=d"(hi), "+c"(fn)
                     : "D"(args)
                     : "esi", "memory", "cc");
    return ((uint64_t)hi << 32) | lo;
}
#else
#error "arm_emu.c requires a 32-bit x86 host"
#endif

static uint32_t *g_svc_page;
static __thread struct arm_emu *t_emu;
static __thread uint32_t t_call_scratch;
static __thread uint32_t t_fault_address;
static __thread uc_mem_type t_fault_type;
static arm_emu_fault_handler g_fault_handler;

static const int k_gpr_ids[16] = {
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,  UC_ARM_REG_R3,  UC_ARM_REG_R4,  UC_ARM_REG_R5,
    UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,  UC_ARM_REG_R9,  UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
};

void arm_emu_set_fault_handler(arm_emu_fault_handler handler) {
    g_fault_handler = handler;
}
static long g_trace_limit;
static _Atomic unsigned long g_host_calls;
static _Atomic unsigned long g_traced;
static _Atomic uint32_t g_last_target;
static uc_engine *_Atomic g_main_uc;
extern size_t uc_debug_full_flushes;
extern size_t uc_debug_notdirty_writes;
extern size_t uc_debug_tlb_fills;
extern size_t uc_debug_tb_flushes;
static bool g_perf_stats;
static _Atomic unsigned long g_hook_calls;
static _Atomic uint64_t g_hook_us;
static _Atomic uint64_t g_host_us;
static __thread bool t_in_host;
static __thread uint64_t t_nested_guest_us;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Pages already confirmed as host code. Every host call checks its target, and the platform
 * lookup is slow (glibc's dladdr scans symbol tables, Windows takes the loader lock): this cache
 * made host calls about a third of the game thread's time cheaper. Loaded modules stay loaded,
 * so entries never go stale. */
enum { HOST_PAGE_SLOTS = 1024 };
static _Atomic uint32_t g_host_pages[HOST_PAGE_SLOTS];

static bool host_code_address(uint32_t address) {
    uint32_t page = (address >> 12) + 1; /* + 1: page 0 never matches an empty slot */
    _Atomic uint32_t *slot = &g_host_pages[page % HOST_PAGE_SLOTS];
    if (*slot == page) {
        return true;
    }
    if (!plat_address_in_module((const void *)(uintptr_t)address)) {
        return false;
    }
    *slot = page;
    return true;
}

static uint32_t g_image_start;
static uint32_t g_image_end;
static bool g_trace_code;

enum { CODE_PAGE_SLOTS = 256 };
static struct {
    uint32_t page;
    unsigned long count;
} g_code_pages[CODE_PAGE_SLOTS];

static void on_block(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    (void)uc;
    (void)size;
    (void)user_data;
    if (address >= g_image_start && address < g_image_end) {
        return;
    }
    uint32_t page = (uint32_t)address & ~0xfffu;
    unsigned index = (page >> 12) % CODE_PAGE_SLOTS;
    for (unsigned probe = 0; probe < CODE_PAGE_SLOTS; ++probe) {
        unsigned slot = (index + probe) % CODE_PAGE_SLOTS;
        if (g_code_pages[slot].page == page || g_code_pages[slot].page == 0) {
            g_code_pages[slot].page = page;
            g_code_pages[slot].count++;
            return;
        }
    }
}

static void report_code_pages(void) {
    for (unsigned i = 0; i < CODE_PAGE_SLOTS; ++i) {
        if (g_code_pages[i].count) {
            fprintf(stderr, "[code] page %08x executed %lu blocks%s\n", g_code_pages[i].page,
                    g_code_pages[i].count,
                    host_code_address(g_code_pages[i].page) ? " (host module shadow)" : "");
            g_code_pages[i].count = 0;
        }
    }
}

enum { CODE_RANGES_MAX = 16 };
static struct {
    uint32_t start;
    uint32_t end;
} g_code_ranges[CODE_RANGES_MAX];
static unsigned g_code_range_count;

void arm_emu_register_code(uint32_t start, uint32_t size) {
    if (g_code_range_count >= CODE_RANGES_MAX || !size) {
        return;
    }
    uint32_t lo = start & ~(uint32_t)(PAGE_SIZE_EMU - 1);
    uint32_t hi = (start + size + PAGE_SIZE_EMU - 1) & ~(uint32_t)(PAGE_SIZE_EMU - 1);
    g_code_ranges[g_code_range_count].start = lo;
    g_code_ranges[g_code_range_count].end = hi;
    g_code_range_count++;
}

static void map_code_ranges(uc_engine *uc) {
    for (unsigned i = 0; i < g_code_range_count; ++i) {
        uint32_t lo = g_code_ranges[i].start;
        uint32_t hi = g_code_ranges[i].end;
        if (uc_mem_map_ptr(uc, lo, hi - lo, UC_PROT_ALL, (void *)(uintptr_t)lo) != UC_ERR_OK) {
            fprintf(stderr, "[arm] unable to map code range %08x-%08x\n", lo, hi);
        }
    }
}

void arm_emu_set_image(uint32_t start, uint32_t end) {
    g_image_start = start;
    g_image_end = end;
    arm_emu_register_code(start, end - start);
}

static void *status_thread(void *arg) {
    (void)arg;
    unsigned long previous = 0;
    for (;;) {
        struct timespec ts = {2, 0};
        nanosleep(&ts, NULL);
        unsigned long now = g_host_calls;
        uint32_t last = g_last_target;
        const char *name = plat_symbol_name((void *)(uintptr_t)last);
        name = name ? name : "?";
        uint32_t pc = 0, lr = 0, regions = 0;
        uc_engine *uc = g_main_uc;
        if (uc) {
            uc_mem_region *list = NULL;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            uc_reg_read(uc, UC_ARM_REG_LR, &lr);
            if (uc_mem_regions(uc, &list, &regions) == UC_ERR_OK) {
                uc_free(list);
            }
        }
        static size_t last_flushes, last_notdirty, last_fills;
        size_t flushes = uc_debug_full_flushes, notdirty = uc_debug_notdirty_writes,
               fills = uc_debug_tlb_fills;
        fprintf(stderr,
                "[status] host calls %lu (+%lu/2s) last=%s@%08x main pc=%08x lr=%08x maps=%u "
                "tlb: +%zu fills +%zu flushes +%zu notdirty\n",
                now, now - previous, name, last, pc, lr, regions, fills - last_fills,
                flushes - last_flushes, notdirty - last_notdirty);
        last_flushes = flushes;
        last_notdirty = notdirty;
        last_fills = fills;
        if (g_trace_code) {
            report_code_pages();
        }
        previous = now;
    }
    return NULL;
}

static uint32_t g_trace_ignored[8];
static unsigned g_trace_ignored_count;

void arm_emu_trace_ignore(uint32_t fn) {
    if (g_trace_ignored_count < sizeof(g_trace_ignored) / sizeof(g_trace_ignored[0])) {
        g_trace_ignored[g_trace_ignored_count++] = fn;
    }
}

static bool trace_call(uint32_t target) {
    ++g_host_calls;
    g_last_target = target;
    if (g_trace_limit <= 0) {
        return false;
    }
    for (unsigned i = 0; i < g_trace_ignored_count; ++i) {
        if (g_trace_ignored[i] == target) {
            return false;
        }
    }
    return (long)++g_traced <= g_trace_limit;
}

static void trace_result(uint32_t target, const uint32_t *a, uint32_t lr, uint64_t result) {
    const char *name = plat_symbol_name((void *)(uintptr_t)target);
    fprintf(stderr, "[call %lu] %s@%08x(%08x, %08x, %08x, %08x) from %08x -> %08x\n",
            (unsigned long)g_traced, name ? name : "?", target, a[0], a[1], a[2], a[3], lr,
            (uint32_t)result);
}

static void clip_to_unmapped(uc_engine *uc, uint32_t address, uint64_t *lo, uint64_t *hi) {
    uc_mem_region *regions = NULL;
    uint32_t count = 0;
    if (uc_mem_regions(uc, &regions, &count) != UC_ERR_OK) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (regions[i].end < address && regions[i].end + 1 > *lo) {
            *lo = regions[i].end + 1;
        }
        if (regions[i].begin > address && regions[i].begin < *hi) {
            *hi = regions[i].begin;
        }
    }
    uc_free(regions);
}

static void *svc_shadow(size_t size) {
    uint32_t *shadow = plat_alloc(NULL, size, false);
    if (!shadow) {
        return NULL;
    }
    for (size_t i = 0; i < size / sizeof(uint32_t); ++i) {
        shadow[i] = SVC_ARM_WORD;
    }
    return shadow;
}


static void report_fault(uc_engine *uc, const char *what, uint64_t address) {
    uint32_t r[16];
    for (int i = 0; i < 16; ++i) {
        uc_reg_read(uc, k_gpr_ids[i], &r[i]);
    }
    fprintf(stderr, "[arm] %s at 0x%08" PRIx64 " pc=%08x lr=%08x sp=%08x\n", what, address, r[15],
            r[14], r[13]);
    /* Image offsets match Ghidra (image loaded at 0x4a000000) and the crash recovery table. */
    if (g_image_start) {
        const char *labels[2] = {"pc", "lr"};
        uint32_t values[2] = {r[15], r[14]};
        for (int i = 0; i < 2; ++i) {
            if (values[i] >= g_image_start && values[i] < g_image_end) {
                fprintf(stderr, "[arm]   %s = image+0x%06x\n", labels[i], values[i] - g_image_start);
            }
        }
    }
    for (int i = 0; i < 13; i += 4) {
        fprintf(stderr, "[arm]   r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n", i, r[i], i + 1,
                r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    }
    uint32_t stack[16];
    if (uc_mem_read(uc, r[13], stack, sizeof(stack)) == UC_ERR_OK) {
        for (int i = 0; i < 16; i += 8) {
            fprintf(stderr, "[arm]   sp+%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4,
                    stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4],
                    stack[i + 5], stack[i + 6], stack[i + 7]);
        }
    }
}

static bool on_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value,
                        void *user_data) {
    (void)size;
    (void)value;
    (void)user_data;
    uint64_t lo, hi;
    bool fetch = type == UC_MEM_FETCH_UNMAPPED;
    bool host_code = fetch && host_code_address((uint32_t)address);
    bool found = fetch ? plat_region((uintptr_t)address, &lo, &hi)
                       : plat_data_window((uintptr_t)address, &lo, &hi);
    if ((uint32_t)address >= PAGE_SIZE_EMU && found) {
        clip_to_unmapped(uc, (uint32_t)address, &lo, &hi);
        size_t length = (size_t)(hi - lo);
        if (host_code) {
            void *shadow = svc_shadow(length);
            if (shadow && uc_mem_map_ptr(uc, lo, length, UC_PROT_READ | UC_PROT_EXEC, shadow) ==
                              UC_ERR_OK) {
                return true;
            }
        } else {
            uint32_t perms = type == UC_MEM_FETCH_UNMAPPED ? UC_PROT_ALL
                                                           : UC_PROT_READ | UC_PROT_WRITE;
            if (uc_mem_map_ptr(uc, lo, length, perms, (void *)(uintptr_t)lo) == UC_ERR_OK) {
                return true;
            }
        }
    }
    /* arm_emu_call reports the fault if no recovery handler takes it. */
    t_fault_address = (uint32_t)address;
    t_fault_type = type;
    return false;
}

/* A flush while a code hook runs (a handler adding or removing hooks) waits until the outermost
 * hook returns: the current translated block is still executing, and nested guest calls from the
 * handler could translate new code over it. The PC is then rewritten to leave the block at once. */
static __thread int t_hook_depth;
static __thread bool t_flush_pending;

/* Hook changes on this thread's CPU are also coalesced: a mod adding twenty hooks in one
 * callback costs one flush (when the host call returns, or before the next guest call), not
 * twenty, which froze the first frames of a modded match. */
static void flush_translations(uc_engine *uc) {
    if (t_emu && uc == t_emu->uc) {
        t_flush_pending = true;
    } else {
        uc_ctl_flush_tb(uc);
    }
}

static void flush_pending_translations(uc_engine *uc) {
    if (t_flush_pending && t_hook_depth == 0) {
        t_flush_pending = false;
        uc_ctl_flush_tb(uc);
    }
}

/* The profiler's guest PC sample: the game thread's last synced PC (the start of the running
 * block, or the guest call site during a host call). */
static pthread_t g_main_thread;

static uint32_t profile_guest_pc(void) {
    uc_engine *uc = g_main_uc;
    uint32_t pc = 0;
    if (uc && pthread_equal(pthread_self(), g_main_thread)) {
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    }
    return pc;
}

static void on_interrupt(uc_engine *uc, uint32_t intno, void *user_data) {
    (void)user_data;
    /* One batch read per host call: the trap runs thousands of times a frame. */
    static const int ids[9] = {UC_ARM_REG_PC, UC_ARM_REG_CPSR, UC_ARM_REG_R12, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2,   UC_ARM_REG_R3,  UC_ARM_REG_SP,
                               UC_ARM_REG_LR};
    uint32_t a[HOST_ARG_WORDS];
    uint32_t pc, cpsr, sp, lr;
    void *values[9] = {&pc, &cpsr, &t_call_scratch, &a[0], &a[1], &a[2], &a[3], &sp, &lr};
    uc_reg_read_batch(uc, ids, values, 9);
    uint32_t target = (cpsr & (1u << 5)) ? ((pc - 2u) | 1u) : pc - 4u;
    if (intno != 2 || !host_code_address(target)) {
        report_fault(uc, "unexpected interrupt", target);
        uc_emu_stop(uc);
        return;
    }

    const uint32_t *stack = (const uint32_t *)(uintptr_t)sp;
    for (int i = 4; i < (int)HOST_ARG_WORDS; ++i) {
        a[i] = stack[i - 4];
    }

    bool traced = trace_call(target);
    if (g_main_uc == NULL) {
        g_main_thread = pthread_self();
        g_main_uc = uc;
        const char *profile = getenv("BOZ_PROFILE");
        if (profile && !strcmp(profile, "all")) {
            plat_profile_guest_pc = profile_guest_pc;
        }
    }
    uint64_t result;
    if (g_perf_stats && !t_in_host && uc == g_main_uc) {
        uint64_t started = now_us(), nested = t_nested_guest_us;
        t_in_host = true;
        result = call_host16(target, a);
        t_in_host = false;
        g_host_us += now_us() - started - (t_nested_guest_us - nested);
    } else {
        result = call_host16(target, a);
    }
    if (traced) {
        trace_result(target, a, lr, result);
    }

    flush_pending_translations(uc);
    uint32_t lo = (uint32_t)result;
    uint32_t hi = (uint32_t)(result >> 32);
    static const int result_ids[2] = {UC_ARM_REG_R0, UC_ARM_REG_R1};
    void *const results[2] = {&lo, &hi};
    uc_reg_write_batch(uc, result_ids, results, 2);
    /* PC on its own: only the single write tells Unicorn not to sync the PC over it on exit. */
    uc_reg_write(uc, UC_ARM_REG_PC, &lr);
}

static struct arm_emu *emu_create(void) {
    struct arm_emu *emu = calloc(1, sizeof(*emu));
    if (!emu) {
        return NULL;
    }
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &emu->uc) != UC_ERR_OK) {
        free(emu);
        return NULL;
    }
    uc_ctl_set_cpu_model(emu->uc, UC_CPU_ARM_CORTEX_A15);

    uint32_t cpacr = 0xfu << 20;
    uint32_t fpexc = 0x40000000u;
    uc_reg_write(emu->uc, UC_ARM_REG_C1_C0_2, &cpacr);
    uc_reg_write(emu->uc, UC_ARM_REG_FPEXC, &fpexc);

    map_code_ranges(emu->uc);

    uc_hook hook;
    uc_hook_add(emu->uc, &hook, UC_HOOK_MEM_UNMAPPED, (void *)on_unmapped, NULL, 1, 0);
    uc_hook_add(emu->uc, &hook, UC_HOOK_INTR, (void *)on_interrupt, NULL, 1, 0);
    if (g_trace_code) {
        uc_hook_add(emu->uc, &hook, UC_HOOK_BLOCK, (void *)on_block, NULL, 1, 0);
    }

    emu->stack = plat_alloc(NULL, STACK_SIZE, false);
    if (!emu->stack) {
        uc_close(emu->uc);
        free(emu);
        return NULL;
    }
    uint32_t sp = (uint32_t)(uintptr_t)(emu->stack + STACK_SIZE - 64);
    uc_reg_write(emu->uc, UC_ARM_REG_SP, &sp);
    emu->sentinel = (uint32_t)(uintptr_t)g_svc_page + PAGE_SIZE_EMU - 4;
    return emu;
}

bool arm_emu_init(void) {
    if (g_svc_page) {
        return true;
    }
    void *page = plat_alloc(NULL, PAGE_SIZE_EMU, false);
    if (!page) {
        fprintf(stderr, "[arm] unable to allocate the svc page\n");
        return false;
    }
    g_svc_page = page;
    arm_emu_register_code((uint32_t)(uintptr_t)page, PAGE_SIZE_EMU);
    const char *trace = getenv("BOZ_TRACE_CALLS");
    g_trace_limit = trace ? strtol(trace, NULL, 10) : 0;
    g_trace_code = getenv("BOZ_TRACE_CODE") != NULL;
    g_perf_stats = getenv("BOZ_PERF_STATS") != NULL;
    if (getenv("BOZ_TRACE_STATUS")) {
        pthread_t thread;
        pthread_create(&thread, NULL, status_thread, NULL);
        pthread_detach(thread);
    }
    for (size_t i = 0; i < PAGE_SIZE_EMU / sizeof(uint32_t); ++i) {
        g_svc_page[i] = SVC_ARM_WORD;
    }
    return true;
}

/* Gives a data fault to the game-specific handler; on success sets *resume to continue from. */
static bool recover_fault(uc_engine *uc, uc_err err, uint64_t *resume) {
    if (!g_fault_handler || (err != UC_ERR_READ_UNMAPPED && err != UC_ERR_WRITE_UNMAPPED)) {
        return false;
    }
    struct arm_emu_fault fault = {
        .address = t_fault_address,
        .write = err == UC_ERR_WRITE_UNMAPPED,
    };
    uint32_t cpsr = 0;
    for (int i = 0; i < 16; ++i) {
        uc_reg_read(uc, k_gpr_ids[i], &fault.r[i]);
    }
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    fault.thumb = (cpsr & (1u << 5)) != 0;
    fault.r[15] &= ~1u;
    uint32_t pc = fault.r[15];
    if (!g_fault_handler(&fault)) {
        return false;
    }
    for (int i = 0; i < 15; ++i) {
        uc_reg_write(uc, k_gpr_ids[i], &fault.r[i]);
    }
    *resume = (fault.r[15] & ~1u) | (fault.thumb ? 1u : 0u);
    static uint32_t reported_pc[16];
    for (int i = 0; i < 16; ++i) {
        if (reported_pc[i] == pc) {
            break;
        }
        if (!reported_pc[i]) {
            reported_pc[i] = pc;
            fprintf(stderr, "[arm] recovered %s fault at 0x%08x (pc=%08x)\n",
                    fault.write ? "write" : "read", fault.address, pc);
            break;
        }
    }
    return true;
}

uint64_t arm_emu_call(uint32_t fn, int argc, const uint32_t *argv) {
    if (!t_emu) {
        t_emu = emu_create();
        if (!t_emu) {
            fprintf(stderr, "[arm] unable to create CPU\n");
            abort();
        }
    }
    uc_engine *uc = t_emu->uc;
    uc_context *saved = NULL;
    if (t_emu->depth > 0) {
        uc_context_alloc(uc, &saved);
        uc_context_save(uc, saved);
    }

    static const int arg_regs[4] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3};
    uint32_t sp;
    uc_reg_read(uc, UC_ARM_REG_SP, &sp);
    int stack_args = argc > 4 ? argc - 4 : 0;
    sp -= (uint32_t)((stack_args * 4 + 7) & ~7);
    for (int i = 0; i < argc; ++i) {
        if (i < 4) {
            uc_reg_write(uc, arg_regs[i], &argv[i]);
        } else {
            ((uint32_t *)(uintptr_t)sp)[i - 4] = argv[i];
        }
    }
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uint32_t lr = t_emu->sentinel;
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);

    flush_pending_translations(uc);
    t_emu->depth++;
    bool nested = t_in_host;
    uint64_t started = nested ? now_us() : 0;
    t_in_host = false;
    uint64_t start = fn;
    uc_err err;
    while ((err = uc_emu_start(uc, start, t_emu->sentinel, 0, 0)) != UC_ERR_OK &&
           recover_fault(uc, err, &start)) {
    }
    if (nested) {
        t_in_host = true;
        t_nested_guest_us += now_us() - started;
    }
    t_emu->depth--;
    if (err != UC_ERR_OK) {
        uint32_t pc;
        uc_reg_read(uc, UC_ARM_REG_PC, &pc);
        if (err == UC_ERR_READ_UNMAPPED || err == UC_ERR_WRITE_UNMAPPED ||
            err == UC_ERR_FETCH_UNMAPPED) {
            report_fault(uc,
                         t_fault_type == UC_MEM_WRITE_UNMAPPED   ? "write to unmapped"
                         : t_fault_type == UC_MEM_FETCH_UNMAPPED ? "fetch from unmapped"
                                                                 : "read from unmapped",
                         t_fault_address);
        }
        fprintf(stderr, "[arm] call 0x%08x stopped: %s (pc=%08x)\n", fn, uc_strerror(err), pc);
        abort();
    }

    uint32_t lo, hi;
    uc_reg_read(uc, UC_ARM_REG_R0, &lo);
    uc_reg_read(uc, UC_ARM_REG_R1, &hi);
    if (saved) {
        uc_context_restore(uc, saved);
        uc_context_free(saved);
    } else {
        sp += (uint32_t)((stack_args * 4 + 7) & ~7);
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    }
    return ((uint64_t)hi << 32) | lo;
}

/* Code hooks: Unicorn code hooks limited to one address, so they cost nothing elsewhere. Adding or
 * removing one flushes the translation cache so code translated before picks the change up
 * (Unicorn's single-range invalidate needs a TLB lookup that fails outside emulation; hooks
 * change rarely, so a full flush is cheap). */
enum { MAX_CODE_HOOKS = 256 };

static struct code_hook {
    uc_engine *uc;
    uc_hook handle;
    uint32_t address;
    arm_emu_code_hook fn;
    void *user;
    bool used;
} g_code_hooks[MAX_CODE_HOOKS];

static void on_code_hook(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    (void)address;
    (void)size;
    struct code_hook *hook = user_data;
    struct arm_emu_regs regs;
    uint32_t cpsr = 0;
    static const int ids[17] = {
        UC_ARM_REG_R0,  UC_ARM_REG_R1,  UC_ARM_REG_R2, UC_ARM_REG_R3,  UC_ARM_REG_R4,
        UC_ARM_REG_R5,  UC_ARM_REG_R6,  UC_ARM_REG_R7, UC_ARM_REG_R8,  UC_ARM_REG_R9,
        UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR,
        UC_ARM_REG_PC,  UC_ARM_REG_CPSR};
    void *values[17];
    for (int i = 0; i < 16; ++i) {
        values[i] = &regs.r[i];
    }
    values[16] = &cpsr;
    uc_reg_read_batch(uc, ids, values, 17);
    regs.thumb = (cpsr & (1u << 5)) != 0;
    regs.r[15] &= ~1u;
    struct arm_emu_regs before = regs;
    bool timed = g_perf_stats && t_hook_depth == 0;
    uint64_t started = timed ? now_us() : 0;
    t_hook_depth++;
    hook->fn(&regs, hook->user);
    t_hook_depth--;
    if (g_perf_stats) {
        ++g_hook_calls;
        if (timed) {
            g_hook_us += now_us() - started;
        }
    }
    bool flush = t_hook_depth == 0 && t_flush_pending;
    if (flush) {
        t_flush_pending = false;
        uc_ctl_flush_tb(uc);
    }
    for (int i = 0; i < 15; ++i) {
        if (regs.r[i] != before.r[i]) {
            uc_reg_write(uc, k_gpr_ids[i], &regs.r[i]);
        }
    }
    if (flush || regs.r[15] != before.r[15] || regs.thumb != before.thumb) {
        uint32_t pc = (regs.r[15] & ~1u) | (regs.thumb ? 1u : 0u);
        uc_reg_write(uc, UC_ARM_REG_PC, &pc);
    }
}

int arm_emu_hook_add(uint32_t address, arm_emu_code_hook fn, void *user) {
    if (!t_emu) {
        t_emu = emu_create();
        if (!t_emu) {
            return -1;
        }
    }
    address &= ~1u;
    for (int id = 0; id < MAX_CODE_HOOKS; ++id) {
        struct code_hook *hook = &g_code_hooks[id];
        if (hook->used) {
            continue;
        }
        *hook = (struct code_hook){t_emu->uc, 0, address, fn, user, true};
        if (uc_hook_add(t_emu->uc, &hook->handle, UC_HOOK_CODE, (void *)on_code_hook, hook,
                        address, address) != UC_ERR_OK) {
            hook->used = false;
            return -1;
        }
        flush_translations(t_emu->uc);
        return id;
    }
    return -1;
}

void arm_emu_hook_remove(int id) {
    if (id < 0 || id >= MAX_CODE_HOOKS || !g_code_hooks[id].used) {
        return;
    }
    struct code_hook *hook = &g_code_hooks[id];
    uc_hook_del(hook->uc, hook->handle);
    flush_translations(hook->uc);
    hook->used = false;
}

void arm_emu_perf_read(struct arm_emu_perf *out) {
    out->host_calls = g_host_calls;
    out->host_us = g_host_us;
    out->hook_calls = g_hook_calls;
    out->hook_us = g_hook_us;
    out->jit_flushes = uc_debug_tb_flushes;
}

uint32_t arm_emu_trap_address(void) {
    /* The last word is the call sentinel; the one before it is free. */
    return g_svc_page ? (uint32_t)(uintptr_t)g_svc_page + PAGE_SIZE_EMU - 8 : 0;
}

uint32_t arm_emu_call_scratch(void) {
    return t_call_scratch;
}

uint64_t arm_emu_call_host(uint32_t fn, const uint32_t *args) {
    return call_host16(fn, args);
}

void arm_emu_thread_exit(void) {
    if (!t_emu) {
        return;
    }
    uc_close(t_emu->uc);
    plat_free(t_emu->stack, STACK_SIZE);
    free(t_emu);
    t_emu = NULL;
}
