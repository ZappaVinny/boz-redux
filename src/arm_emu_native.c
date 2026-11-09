/* The ARM CPU on 32-bit ARM hosts (Android armeabi-v7a TVs): no emulator, the game's code runs on
 * the host CPU, as on the phones the game was made for. Guest and host share the address space and
 * the calling convention (AAPCS, soft-float), so guest calls into host functions and host calls
 * into guest code are plain native calls.
 *
 * What an emulator gave for free is done with signals instead:
 * - Code hooks write an undefined instruction (UDF) over the hooked one. SIGILL runs the hooks;
 *   the original instruction then runs from a trampoline (copied, followed by a jump back), or is
 *   emulated in the handler when it depends on its own address (branches, literal loads, ADR).
 * - Guest memory faults (SIGSEGV/SIGBUS with the PC outside host modules) go to the fault handler
 *   with the exact registers, and resume where it says.
 * - Guest code in memory that is not executable yet (Unicorn ran any mapped memory) faults on
 *   the fetch; the pages are made executable and the instruction runs again.
 * Faults and SIGILLs that are not the game's go on to the previous handler (ART's, or the
 * default crash), so host crashes still leave a tombstone. */
#include "arm_emu.h"

#include "platform/platform.h"

#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(__arm__)
#error "arm_emu_native.c runs the game's code directly: it needs a 32-bit ARM host"
#endif

/* UDF encodings the kernel leaves alone (its own breakpoints are 0xe7f001f0/0xde01/0xf7f0a000). */
#define UDF_ARM 0xe7f0bef0u
#define UDF_THUMB 0xdebeu
#define CPSR_T (1u << 5)
#define CPSR_IT_MASK 0x0600fc00u

/* Optional: the gamedef says whether a function is Thumb (game_image.c). Absent in tests. */
int game_code_is_thumb(uint32_t address) __attribute__((weak));

typedef uint64_t (*host16_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                              uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                              uint32_t, uint32_t, uint32_t, uint32_t);

static uint64_t call16(uint32_t fn, const uint32_t *a) {
    return ((host16_fn)(uintptr_t)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9],
                                      a[10], a[11], a[12], a[13], a[14], a[15]);
}

static size_t g_page;
static arm_emu_fault_handler g_fault_handler;
static uint32_t g_image_start, g_image_end;
static bool g_perf_stats;
static _Atomic unsigned long g_hook_calls;
static _Atomic uint64_t g_hook_us;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uintptr_t page_floor(uintptr_t address) {
    return address & ~(uintptr_t)(g_page - 1);
}

/* Read, write and execute: the game writes its own data next to its code, and patches here. */
static bool make_rwx(uintptr_t start, uintptr_t end) {
    uintptr_t lo = page_floor(start), hi = page_floor(end + g_page - 1);
    return mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

static void sync_code(uintptr_t start, uintptr_t end) {
    __builtin___clear_cache((char *)start, (char *)end);
}

/* --- Code ranges: made executable, their instruction cache synced before guest code runs ------ */

enum { CODE_RANGES_MAX = 32 };
static struct {
    uint32_t start, end;
} g_ranges[CODE_RANGES_MAX];
static unsigned g_range_count;
static atomic_bool g_ranges_dirty;

void arm_emu_register_code(uint32_t start, uint32_t size) {
    if (!size) {
        return;
    }
    if (!make_rwx(start, (uintptr_t)start + size)) {
        fprintf(stderr, "[arm] cannot make %08x-%08x executable\n", start, start + size);
    }
    pthread_mutex_lock(&g_lock);
    if (g_range_count < CODE_RANGES_MAX) {
        g_ranges[g_range_count].start = start;
        g_ranges[g_range_count].end = start + size;
        g_range_count++;
    }
    pthread_mutex_unlock(&g_lock);
    g_ranges_dirty = true;
}

/* The loader writes code (the image, relocations, stubs) as data: sync it once before running. */
static void sync_ranges(void) {
    if (!atomic_exchange(&g_ranges_dirty, false)) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < g_range_count; ++i) {
        sync_code(g_ranges[i].start, g_ranges[i].end);
    }
    pthread_mutex_unlock(&g_lock);
}

void arm_emu_set_image(uint32_t start, uint32_t end) {
    g_image_start = start;
    g_image_end = end;
    arm_emu_register_code(start, end - start);
}

void arm_emu_trace_ignore(uint32_t fn) {
    (void)fn;
}

/* --- Executable scratch: trampolines, the trap page and r12 thunks --------------------------- */

enum { EXEC_POOL_SIZE = 64 * 1024 };
static uint8_t *g_pool;
static size_t g_pool_used;
static uint32_t *g_trap_page;

static void *pool_alloc(size_t size) {
    size = (size + 7) & ~(size_t)7;
    if (!g_pool || g_pool_used + size > EXEC_POOL_SIZE) {
        return NULL;
    }
    void *out = g_pool + g_pool_used;
    g_pool_used += size;
    return out;
}

static bool in_pool(uint32_t address) {
    return g_pool && address >= (uint32_t)(uintptr_t)g_pool &&
           address < (uint32_t)(uintptr_t)g_pool + EXEC_POOL_SIZE;
}

/* r12 at the moment guest code called a host function: the lazy GL stubs pass their index in it.
 * Compiled code may use r12 before reading it, so arm_emu_guest_fn hands out a thunk that saves
 * it first. */
static __thread uint32_t t_call_scratch;

static void save_scratch(uint32_t value) {
    t_call_scratch = value;
}

uint32_t arm_emu_call_scratch(void) {
    return t_call_scratch;
}

enum { THUNKS_MAX = 64 };
static struct {
    uint32_t fn, thunk;
} g_thunks[THUNKS_MAX];
static unsigned g_thunk_count;

uint32_t arm_emu_guest_fn(const void *fn) {
    uint32_t target = (uint32_t)(uintptr_t)fn;
    uint32_t found = 0;
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < g_thunk_count && !found; ++i) {
        if (g_thunks[i].fn == target) {
            found = g_thunks[i].thunk;
        }
    }
    uint32_t *code = found || g_thunk_count >= THUNKS_MAX ? NULL : pool_alloc(8 * sizeof(uint32_t));
    if (code) {
        code[0] = 0xe92d500fu; /* push {r0-r3, r12, lr} (keeps sp 8-byte aligned) */
        code[1] = 0xe1a0000cu; /* mov r0, r12 */
        code[2] = 0xe59fc00cu; /* ldr r12, [pc, #12]  -> code[7] */
        code[3] = 0xe12fff3cu; /* blx r12 */
        code[4] = 0xe8bd500fu; /* pop {r0-r3, r12, lr} */
        code[5] = 0xe51ff004u; /* ldr pc, [pc, #-4]   -> code[6] */
        code[6] = target;
        code[7] = (uint32_t)(uintptr_t)&save_scratch;
        sync_code((uintptr_t)code, (uintptr_t)(code + 8));
        found = (uint32_t)(uintptr_t)code;
        g_thunks[g_thunk_count].fn = target;
        g_thunks[g_thunk_count].thunk = found;
        g_thunk_count++;
    }
    pthread_mutex_unlock(&g_lock);
    return found ? found : target;
}

uint32_t arm_emu_guest_fn_sig(const void *fn, const char *signature) {
    (void)signature;
    return (uint32_t)(uintptr_t)fn;
}

uint32_t arm_emu_trap_address(void) {
    return g_trap_page ? (uint32_t)(uintptr_t)g_trap_page : 0;
}

/* --- Calls ------------------------------------------------------------------------------------ */

uint64_t arm_emu_call(uint32_t fn, int argc, const uint32_t *argv) {
    uint32_t a[16] = {0};
    if (argc > 16) {
        fprintf(stderr, "[arm] call 0x%08x with %d arguments (16 at most)\n", fn, argc);
        abort();
    }
    for (int i = 0; i < argc; ++i) {
        a[i] = argv[i];
    }
    sync_ranges();
    return call16(fn, a);
}

uint64_t arm_emu_call_host(uint32_t fn, const uint32_t *args) {
    return call16(fn, args);
}

void arm_emu_thread_exit(void) {}

void arm_emu_set_fault_handler(arm_emu_fault_handler handler) {
    g_fault_handler = handler;
}

void arm_emu_perf_read(struct arm_emu_perf *out) {
    out->host_calls = 0; /* direct calls: nothing to count */
    out->host_us = 0;
    out->hook_calls = g_hook_calls;
    out->hook_us = g_hook_us;
    out->jit_flushes = 0;
}

/* --- Instruction decoding for displaced execution ------------------------------------------- */

enum kind { KIND_REJECT, KIND_COPY, KIND_EMULATE, KIND_TRAP };

static bool is_thumb32(uint16_t first) {
    unsigned top = first >> 11;
    return top == 0x1d || top == 0x1e || top == 0x1f;
}

static int classify_arm(uint32_t i) {
    unsigned cond = i >> 28, op = (i >> 25) & 7;
    unsigned rn = (i >> 16) & 15, rd = (i >> 12) & 15, rm = i & 15;
    if (cond == 0xf) {
        if (op == 5) {
            return KIND_EMULATE; /* BLX immediate */
        }
        if ((i & 0xfd70f000u) == 0xf550f000u && rn != 15) {
            return KIND_COPY; /* PLD */
        }
        return KIND_REJECT;
    }
    switch (op) {
    case 5:
        return KIND_EMULATE; /* B, BL */
    case 4:                  /* LDM/STM */
        if (rn == 15 || (!(i & (1u << 20)) && (i & (1u << 15)))) {
            return KIND_REJECT;
        }
        return KIND_COPY;
    case 2:
    case 3: /* LDR/STR (and media instructions with bit 4) */
        if (op == 3 && (i & 0x10u)) {
            return rn == 15 || rd == 15 || rm == 15 ? KIND_REJECT : KIND_COPY;
        }
        if (rn == 15) {
            /* LDR/LDRB rd, [pc, #imm]: offset addressing, a load, not into pc */
            bool literal = op == 2 && (i & (1u << 20)) && (i & (1u << 24)) && !(i & (1u << 21));
            return literal && rd != 15 ? KIND_EMULATE : KIND_REJECT;
        }
        if ((op == 3 && rm == 15) || (rd == 15 && !(i & (1u << 20)))) {
            return KIND_REJECT;
        }
        return KIND_COPY;
    case 6: /* coprocessor loads and stores: VLDR/VSTR/VPUSH... */
        return rn == 15 ? KIND_REJECT : KIND_COPY;
    case 7: /* coprocessor data, SVC */
        return KIND_COPY;
    default: { /* data processing, multiplies, BX/BLX register, halfword loads */
        if ((i & 0x0ffffff0u) == 0x012fff10u || (i & 0x0ffffff0u) == 0x012fff30u) {
            return rm == 15 ? KIND_REJECT : KIND_COPY; /* BX, BLX register */
        }
        unsigned opcode = (i >> 21) & 15;
        bool data = (i & 0x0c000000u) == 0 && (op == 1 || (i & 0x90u) != 0x90u);
        bool movw = (i & 0x0fb00000u) == 0x03000000u; /* MOVW/MOVT: bits 19-16 are immediate */
        if (op == 1 && data && !movw && rn == 15 && rd != 15 && !(i & (1u << 20)) &&
            (opcode == 4 || opcode == 2)) {
            return KIND_EMULATE; /* ADR: ADD/SUB rd, pc, #imm */
        }
        bool uses_rn = data && !movw && opcode != 13 && opcode != 15; /* MOV, MVN have no Rn */
        if ((uses_rn && rn == 15) || (op == 0 && rm == 15) || (!data && (rn == 15 || rd == 15))) {
            return KIND_REJECT;
        }
        if (rd == 15 && data && (i & (1u << 20))) {
            return KIND_REJECT; /* SUBS pc, lr: exception return */
        }
        return KIND_COPY;
    }
    }
}

static int classify_thumb16(uint16_t h) {
    if ((h & 0xf800u) == 0x4800u || (h & 0xf800u) == 0xa000u || (h & 0xf800u) == 0xe000u) {
        return KIND_EMULATE; /* LDR literal, ADR, B */
    }
    if ((h & 0xf000u) == 0xd000u) {
        unsigned cond = (h >> 8) & 15;
        return cond == 0xe ? KIND_REJECT : cond == 0xf ? KIND_COPY : KIND_EMULATE; /* B<c>, SVC */
    }
    if ((h & 0xf500u) == 0xb100u) {
        return KIND_EMULATE; /* CBZ, CBNZ */
    }
    if ((h & 0xff00u) == 0xbf00u && (h & 0xfu)) {
        return KIND_REJECT; /* IT: the block would run unconditionally */
    }
    if ((h & 0xfc00u) == 0x4400u) { /* ADD/CMP/MOV high registers, BX/BLX */
        unsigned op = (h >> 8) & 3, rm = (h >> 3) & 15, rdn = ((h >> 4) & 8) | (h & 7);
        if (op == 3) {
            return rm == 15 ? KIND_REJECT : KIND_COPY;
        }
        if (rm == 15 || (rdn == 15 && op != 2)) {
            return KIND_REJECT;
        }
    }
    return KIND_COPY;
}

static int classify_thumb32(uint16_t h1, uint16_t h2) {
    unsigned rn = h1 & 15;
    if ((h1 & 0xf800u) == 0xf000u && (h2 & 0x8000u)) {
        bool branch = (h2 & 0xd000u) == 0x9000u || (h2 & 0xd000u) == 0xd000u ||
                      (h2 & 0xd001u) == 0xc000u ||
                      ((h2 & 0xd000u) == 0x8000u && ((h1 >> 7) & 7) != 7);
        return branch ? KIND_EMULATE : KIND_REJECT;
    }
    if ((h1 & 0xff7fu) == 0xf85fu || (h1 & 0xff7fu) == 0xf81fu || (h1 & 0xff7fu) == 0xf83fu ||
        (h1 & 0xff7fu) == 0xf91fu || (h1 & 0xff7fu) == 0xf93fu) {
        return KIND_EMULATE; /* LDR/LDRB/LDRH/LDRSB/LDRSH literal */
    }
    if ((h1 & 0xfbffu) == 0xf20fu || (h1 & 0xfbffu) == 0xf2afu) {
        return KIND_EMULATE; /* ADR.W */
    }
    if ((h1 & 0xfa00u) == 0xf000u && !(h2 & 0x8000u)) { /* data processing, modified immediate */
        unsigned op = (h1 >> 5) & 15;
        if ((op == 2 || op == 3) && rn == 15) {
            return KIND_COPY; /* MOV/MVN */
        }
    }
    if ((h1 & 0xfe00u) == 0xea00u) { /* data processing, shifted register */
        unsigned op = (h1 >> 5) & 15;
        if ((h2 & 15) == 15) {
            return KIND_REJECT;
        }
        if ((op == 2 || op == 3) && rn == 15) {
            return KIND_COPY; /* MOV/MVN */
        }
    }
    if ((h1 & 0xfbf0u) == 0xf240u || (h1 & 0xfbf0u) == 0xf2c0u) {
        return KIND_COPY; /* MOVW/MOVT */
    }
    return rn == 15 ? KIND_REJECT : KIND_COPY;
}

static bool condition_passes(unsigned cond, uint32_t cpsr) {
    bool n = cpsr >> 31 & 1, z = cpsr >> 30 & 1, c = cpsr >> 29 & 1, v = cpsr >> 28 & 1;
    switch (cond) {
    case 0: return z;
    case 1: return !z;
    case 2: return c;
    case 3: return !c;
    case 4: return n;
    case 5: return !n;
    case 6: return v;
    case 7: return !v;
    case 8: return c && !z;
    case 9: return !c || z;
    case 10: return n == v;
    case 11: return n != v;
    case 12: return !z && n == v;
    case 13: return z || n != v;
    default: return true;
    }
}

static int32_t sign_extend(uint32_t value, unsigned bits) {
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((value ^ m) - m);
}

/* --- Hook sites ------------------------------------------------------------------------------ */

enum { MAX_SITES = 256, MAX_HOOKS = 256 };

static struct site {
    atomic_bool used;
    uint32_t address;
    bool thumb;
    uint8_t size;
    uint16_t h1, h2; /* Thumb halfwords */
    uint32_t word;   /* the ARM instruction */
    int kind;
    uint32_t trampoline; /* KIND_COPY; with the Thumb bit for Thumb */
    int hooks;
} g_sites[MAX_SITES];

static struct hook {
    atomic_bool used;
    int site;
    arm_emu_code_hook fn;
    void *user;
    pthread_t owner;
} g_hooks[MAX_HOOKS];

static struct site *find_site(uint32_t address) {
    for (int i = 0; i < MAX_SITES; ++i) {
        if (atomic_load(&g_sites[i].used) && g_sites[i].address == address) {
            return &g_sites[i];
        }
    }
    return NULL;
}

/* A site whose trampoline holds pc: faults there are reported at the hooked instruction. */
static struct site *site_of_trampoline(uint32_t pc) {
    for (int i = 0; i < MAX_SITES; ++i) {
        struct site *s = &g_sites[i];
        uint32_t t = s->trampoline & ~1u;
        if (atomic_load(&s->used) && s->kind == KIND_COPY && pc >= t && pc < t + s->size) {
            return s;
        }
    }
    return NULL;
}

static uint32_t build_trampoline(const struct site *s) {
    if (s->thumb) {
        uint16_t *code = pool_alloc(12);
        if (!code) {
            return 0;
        }
        code[0] = s->h1;
        code[1] = s->size == 4 ? s->h2 : 0xbf00u; /* nop: the jump below must be word aligned */
        code[2] = 0xf8dfu;                        /* ldr.w pc, [pc, #0] */
        code[3] = 0xf000u;
        uint32_t back = (s->address + s->size) | 1u;
        memcpy(&code[4], &back, sizeof(back));
        sync_code((uintptr_t)code, (uintptr_t)code + 12);
        return (uint32_t)(uintptr_t)code | 1u;
    }
    uint32_t *code = pool_alloc(12);
    if (!code) {
        return 0;
    }
    code[0] = s->word;
    code[1] = 0xe51ff004u; /* ldr pc, [pc, #-4] */
    code[2] = s->address + 4;
    sync_code((uintptr_t)code, (uintptr_t)code + 12);
    return (uint32_t)(uintptr_t)code;
}

static bool write_code(uint32_t address, const void *bytes, size_t size) {
    if (!make_rwx(address, (uintptr_t)address + size)) {
        return false;
    }
    memcpy((void *)(uintptr_t)address, bytes, size);
    sync_code(address, (uintptr_t)address + size);
    return true;
}

/* Whether the code at address is Thumb: the caller's Thumb bit, else the gamedef, else an
 * address that is not word aligned. */
static bool code_is_thumb(uint32_t address) {
    if (address & 1u) {
        return true;
    }
    if (game_code_is_thumb) {
        int known = game_code_is_thumb(address);
        if (known >= 0) {
            return known != 0;
        }
    }
    return (address & 2u) != 0;
}

static struct site *open_site(uint32_t address_with_mode) {
    uint32_t address = address_with_mode & ~1u;
    struct site *s = find_site(address);
    if (s) {
        return s;
    }
    for (int i = 0; i < MAX_SITES; ++i) {
        if (atomic_load(&g_sites[i].used)) {
            continue;
        }
        s = &g_sites[i];
        memset((void *)&s->address, 0, sizeof(*s) - offsetof(struct site, address));
        s->address = address;
        if (g_trap_page && address >= (uint32_t)(uintptr_t)g_trap_page &&
            address < (uint32_t)(uintptr_t)g_trap_page + g_page) {
            s->kind = KIND_TRAP; /* already a UDF; its hook must move on */
            s->size = 4;
        } else if (code_is_thumb(address_with_mode)) {
            s->thumb = true;
            memcpy(&s->h1, (const void *)(uintptr_t)address, 2);
            s->size = is_thumb32(s->h1) ? 4 : 2;
            if (s->size == 4) {
                memcpy(&s->h2, (const void *)(uintptr_t)(address + 2), 2);
            }
            s->kind = s->size == 4 ? classify_thumb32(s->h1, s->h2) : classify_thumb16(s->h1);
        } else {
            memcpy(&s->word, (const void *)(uintptr_t)address, 4);
            s->size = 4;
            s->kind = classify_arm(s->word);
        }
        if (s->kind == KIND_REJECT) {
            fprintf(stderr, "[arm] cannot hook %08x: %s instruction %08x reads its own address\n",
                    address, s->thumb ? "Thumb" : "ARM",
                    s->thumb ? (uint32_t)s->h1 << 16 | s->h2 : s->word);
            return NULL;
        }
        if (s->kind == KIND_COPY && !(s->trampoline = build_trampoline(s))) {
            fprintf(stderr, "[arm] out of trampoline space\n");
            return NULL;
        }
        if (s->kind != KIND_TRAP) {
            bool ok;
            if (s->thumb) {
                uint16_t udf = UDF_THUMB;
                ok = write_code(address, &udf, 2);
            } else {
                uint32_t udf = UDF_ARM;
                ok = write_code(address, &udf, 4);
            }
            if (!ok) {
                fprintf(stderr, "[arm] cannot patch %08x\n", address);
                return NULL;
            }
        }
        atomic_store(&s->used, true);
        return s;
    }
    return NULL;
}

static void close_site(struct site *s) {
    if (s->kind != KIND_TRAP) {
        if (s->thumb) {
            write_code(s->address, &s->h1, 2);
        } else {
            write_code(s->address, &s->word, 4);
        }
    }
    atomic_store(&s->used, false); /* its trampoline stays: a thread may still be in it */
}

int arm_emu_hook_add(uint32_t address, arm_emu_code_hook fn, void *user) {
    if (!g_pool) {
        return -1;
    }
    int id = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_HOOKS; ++i) {
        if (!atomic_load(&g_hooks[i].used)) {
            id = i;
            break;
        }
    }
    struct site *s = id >= 0 ? open_site(address) : NULL;
    if (s) {
        struct hook *h = &g_hooks[id];
        h->site = (int)(s - g_sites);
        h->fn = fn;
        h->user = user;
        h->owner = pthread_self();
        s->hooks++;
        atomic_store(&h->used, true);
    } else {
        id = -1;
    }
    pthread_mutex_unlock(&g_lock);
    return id;
}

void arm_emu_hook_remove(int id) {
    if (id < 0 || id >= MAX_HOOKS) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    struct hook *h = &g_hooks[id];
    if (atomic_load(&h->used)) {
        atomic_store(&h->used, false);
        struct site *s = &g_sites[h->site];
        if (--s->hooks == 0) {
            close_site(s);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* --- Signals ---------------------------------------------------------------------------------- */

static struct sigaction g_old_ill, g_old_segv, g_old_bus;

/* r0-r15 then cpsr, as the kernel lays out struct sigcontext. */
static uint32_t *context_regs(void *context) {
    return (uint32_t *)&((ucontext_t *)context)->uc_mcontext.arm_r0;
}

static void set_pc(uint32_t *r, uint32_t target, bool thumb) {
    r[15] = target & ~1u;
    r[16] = (r[16] & ~(CPSR_T | CPSR_IT_MASK)) | (thumb ? CPSR_T : 0);
}

static void pass_on(int signal, siginfo_t *info, void *context, const struct sigaction *old) {
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
        old->sa_sigaction(signal, info, context);
        return;
    }
    if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(signal);
        return;
    }
    /* Default action: put it back and return; the instruction faults again and ends the game
     * with the system's crash report. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(signal, &dfl, NULL);
}

/* Runs the hooked instruction's effect when it depends on its own address. */
static void emulate(const struct site *s, uint32_t *r) {
    uint32_t a = s->address, cpsr = r[16];
    if (!s->thumb) {
        uint32_t i = s->word, pc = a + 8;
        unsigned cond = i >> 28, op = (i >> 25) & 7, rd = (i >> 12) & 15;
        if (cond == 0xf) { /* BLX immediate */
            uint32_t target = pc + (uint32_t)(sign_extend(i & 0xffffffu, 24) << 2) + ((i >> 23) & 2u);
            r[14] = a + 4;
            set_pc(r, target, true);
            return;
        }
        if (!condition_passes(cond, cpsr)) {
            set_pc(r, a + 4, false);
            return;
        }
        if (op == 5) { /* B, BL */
            if (i & (1u << 24)) {
                r[14] = a + 4;
            }
            set_pc(r, pc + (uint32_t)(sign_extend(i & 0xffffffu, 24) << 2), false);
            return;
        }
        if (op == 2) { /* LDR/LDRB literal */
            uint32_t offset = i & 0xfffu;
            uint32_t at = (i & (1u << 23)) ? pc + offset : pc - offset;
            r[rd] = (i & (1u << 22)) ? *(const uint8_t *)(uintptr_t)at
                                     : *(const uint32_t *)(uintptr_t)at;
        } else { /* ADR */
            unsigned rotate = ((i >> 8) & 15) * 2;
            uint32_t imm = i & 0xffu;
            imm = rotate ? (imm >> rotate) | (imm << (32 - rotate)) : imm;
            r[rd] = ((i >> 21) & 15) == 4 ? pc + imm : pc - imm;
        }
        set_pc(r, a + 4, false);
        return;
    }
    uint16_t h = s->h1;
    uint32_t pc = a + 4, aligned = pc & ~3u;
    if (s->size == 2) {
        if ((h & 0xf800u) == 0x4800u) {
            r[(h >> 8) & 7] = *(const uint32_t *)(uintptr_t)(aligned + (h & 0xffu) * 4);
        } else if ((h & 0xf800u) == 0xa000u) {
            r[(h >> 8) & 7] = aligned + (h & 0xffu) * 4;
        } else if ((h & 0xf800u) == 0xe000u) {
            set_pc(r, pc + (uint32_t)(sign_extend(h & 0x7ffu, 11) << 1), true);
            return;
        } else if ((h & 0xf000u) == 0xd000u) {
            uint32_t target = condition_passes((h >> 8) & 15, cpsr)
                                  ? pc + (uint32_t)(sign_extend(h & 0xffu, 8) << 1)
                                  : a + 2;
            set_pc(r, target, true);
            return;
        } else { /* CBZ, CBNZ */
            uint32_t offset = ((h >> 9) & 1u) << 6 | ((h >> 3) & 0x1fu) << 1;
            bool zero = r[h & 7] == 0, taken = (h & 0x800u) ? !zero : zero;
            set_pc(r, taken ? pc + offset : a + 2, true);
            return;
        }
        set_pc(r, a + 2, true);
        return;
    }
    uint16_t h2 = s->h2;
    if ((h & 0xf800u) == 0xf000u && (h2 & 0x8000u)) {
        uint32_t sbit = (h >> 10) & 1u, j1 = (h2 >> 13) & 1u, j2 = (h2 >> 11) & 1u;
        if ((h2 & 0xd000u) == 0x8000u) { /* B<c>.W */
            uint32_t imm = sbit << 20 | j2 << 19 | j1 << 18 | (h & 0x3fu) << 12 | (h2 & 0x7ffu) << 1;
            uint32_t target = condition_passes((h >> 6) & 15, cpsr)
                                  ? pc + (uint32_t)sign_extend(imm, 21)
                                  : a + 4;
            set_pc(r, target, true);
            return;
        }
        uint32_t i1 = !(j1 ^ sbit), i2 = !(j2 ^ sbit);
        uint32_t imm = sbit << 24 | i1 << 23 | i2 << 22 | (h & 0x3ffu) << 12 | (h2 & 0x7ffu) << 1;
        uint32_t offset = (uint32_t)sign_extend(imm, 25);
        if ((h2 & 0xd000u) == 0x9000u) { /* B.W */
            set_pc(r, pc + offset, true);
        } else if ((h2 & 0xd000u) == 0xd000u) { /* BL */
            r[14] = (a + 4) | 1u;
            set_pc(r, pc + offset, true);
        } else { /* BLX to ARM */
            r[14] = (a + 4) | 1u;
            set_pc(r, (aligned + offset) & ~3u, false);
        }
        return;
    }
    if ((h & 0xfbffu) == 0xf20fu || (h & 0xfbffu) == 0xf2afu) { /* ADR.W */
        uint32_t imm = ((h >> 10) & 1u) << 11 | ((h2 >> 12) & 7u) << 8 | (h2 & 0xffu);
        r[(h2 >> 8) & 15] = (h & 0x00a0u) == 0x00a0u ? aligned - imm : aligned + imm;
        set_pc(r, a + 4, true);
        return;
    }
    /* LDR/LDRB/LDRH/LDRSB/LDRSH literal */
    uint32_t offset = h2 & 0xfffu, rt = h2 >> 12;
    uint32_t at = (h & 0x80u) ? aligned + offset : aligned - offset;
    uint32_t value;
    switch (h & 0xff7fu) {
    case 0xf81fu: value = *(const uint8_t *)(uintptr_t)at; break;
    case 0xf83fu: value = *(const uint16_t *)(uintptr_t)at; break;
    case 0xf91fu: value = (uint32_t)(int32_t)*(const int8_t *)(uintptr_t)at; break;
    case 0xf93fu: value = (uint32_t)(int32_t)*(const int16_t *)(uintptr_t)at; break;
    default: value = *(const uint32_t *)(uintptr_t)at; break;
    }
    if (rt == 15) {
        if ((h & 0xff7fu) == 0xf85fu) {
            set_pc(r, value, value & 1u); /* LDR pc, literal: a jump */
        } else {
            set_pc(r, a + 4, true); /* PLD/PLI literal: a hint */
        }
        return;
    }
    r[rt] = value;
    set_pc(r, a + 4, true);
}

static void on_sigill(int signal, siginfo_t *info, void *context) {
    uint32_t *r = context_regs(context);
    uint32_t pc = r[15] & ~1u;
    struct site *s = find_site(pc);
    if (!s) {
        pass_on(signal, info, context, &g_old_ill);
        return;
    }
    struct arm_emu_regs regs;
    memcpy(regs.r, r, sizeof(regs.r));
    regs.r[15] = pc;
    regs.thumb = (r[16] & CPSR_T) != 0;
    struct arm_emu_regs before = regs;
    pthread_t self = pthread_self();
    for (int i = 0; i < MAX_HOOKS; ++i) {
        struct hook *h = &g_hooks[i];
        if (!atomic_load(&h->used) || &g_sites[h->site] != s || !pthread_equal(h->owner, self)) {
            continue;
        }
        uint64_t started = g_perf_stats ? now_us() : 0;
        h->fn(&regs, h->user);
        if (g_perf_stats) {
            ++g_hook_calls;
            g_hook_us += now_us() - started;
        }
        if (regs.r[15] != before.r[15] || regs.thumb != before.thumb) {
            break; /* moved on: the hooks after it do not run here (as with Unicorn) */
        }
    }
    memcpy(r, regs.r, 15 * sizeof(uint32_t));
    if (regs.r[15] != before.r[15] || regs.thumb != before.thumb) {
        set_pc(r, regs.r[15], regs.thumb);
        return;
    }
    switch (s->kind) {
    case KIND_COPY:
        set_pc(r, s->trampoline, s->thumb);
        return;
    case KIND_EMULATE:
        emulate(s, r);
        return;
    default:
        fprintf(stderr, "[arm] return trap at %08x reached without a hook moving on\n", pc);
        abort();
    }
}

static void report_fault(const uint32_t *r, uint32_t address) {
    fprintf(stderr, "[arm] fault at 0x%08x pc=%08x lr=%08x sp=%08x\n", address, r[15], r[14], r[13]);
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
}

static void on_fault(int signal, siginfo_t *info, void *context) {
    const struct sigaction *old = signal == SIGBUS ? &g_old_bus : &g_old_segv;
    uint32_t *r = context_regs(context);
    uint32_t pc = r[15] & ~1u;
    if (plat_address_in_module((const void *)(uintptr_t)pc)) {
        pass_on(signal, info, context, old); /* a host crash */
        return;
    }
    uint32_t address = (uint32_t)(uintptr_t)info->si_addr;
    /* Guest code in memory that is not executable yet: the fetch itself faulted. Once per
     * instruction: a second fault there is a real one. */
    static __thread uint32_t t_made_executable;
    if (signal == SIGSEGV && address >= pc && address < pc + 4 && !in_pool(pc) &&
        t_made_executable != pc && make_rwx(pc, pc + 4)) {
        t_made_executable = pc;
        sync_code(page_floor(pc), page_floor(pc + 4) + g_page);
        return;
    }
    /* A fault in a hooked instruction's trampoline is the hooked instruction's. */
    struct site *s = in_pool(pc) ? site_of_trampoline(pc) : NULL;
    struct arm_emu_fault fault;
    memcpy(fault.r, r, sizeof(fault.r));
    fault.r[15] = s ? s->address : pc;
    fault.thumb = s ? s->thumb : (r[16] & CPSR_T) != 0;
    fault.address = address;
    fault.write = (((ucontext_t *)context)->uc_mcontext.error_code & (1u << 11)) != 0;
    uint32_t reported = fault.r[15];
    if (g_fault_handler && g_fault_handler(&fault)) {
        memcpy(r, fault.r, 15 * sizeof(uint32_t));
        if (fault.r[15] == reported && fault.thumb == (s ? s->thumb : (r[16] & CPSR_T) != 0)) {
            r[15] = pc; /* retry the instruction (in the trampoline when it was there) */
        } else {
            set_pc(r, fault.r[15], fault.thumb);
        }
        static uint32_t reported_pc[16];
        for (int i = 0; i < 16; ++i) {
            if (reported_pc[i] == reported) {
                break;
            }
            if (!reported_pc[i]) {
                reported_pc[i] = reported;
                fprintf(stderr, "[arm] recovered %s fault at 0x%08x (pc=%08x)\n",
                        fault.write ? "write" : "read", address, reported);
                break;
            }
        }
        return;
    }
    fault.r[15] = reported;
    report_fault(fault.r, address);
    pass_on(signal, info, context, old);
}

bool arm_emu_init(void) {
    if (g_pool) {
        return true;
    }
    g_page = (size_t)sysconf(_SC_PAGESIZE);
    g_pool = mmap(NULL, EXEC_POOL_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_pool == MAP_FAILED) {
        g_pool = NULL;
        fprintf(stderr, "[arm] cannot allocate executable memory\n");
        return false;
    }
    g_trap_page = pool_alloc(g_page); /* the pool's first page */
    for (size_t i = 0; i < g_page / 4; ++i) {
        g_trap_page[i] = UDF_ARM;
    }
    sync_code((uintptr_t)g_trap_page, (uintptr_t)g_trap_page + g_page);
    g_perf_stats = getenv("BOZ_PERF_STATS") != NULL;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    /* SA_NODEFER: hooks call guest code, which may hit other hooks and faults. */
    action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
    action.sa_sigaction = on_sigill;
    sigaction(SIGILL, &action, &g_old_ill);
    action.sa_sigaction = on_fault;
    sigaction(SIGSEGV, &action, &g_old_segv);
    sigaction(SIGBUS, &action, &g_old_bus);
    fprintf(stderr, "[arm] running game code natively\n");
    return true;
}
