/* Memory the game can see (include/guest_memory.h): the guest space, its pages, the guest heap
 * and per-thread scratch space for callback data.
 *
 * The heap (64-bit hosts) is a boundary-tag allocator with segregated free lists over one
 * region. It works in guest addresses, so the same code serves
 * the identity map and the guest-base map. Payloads are 16-byte aligned, as the game's allocator
 * expects. */
#include "guest_memory.h"
#include "platform/platform.h"

#include <pthread.h>
#include <stdio.h>

#ifdef BOZ_GUEST_BASE
#if defined(_WIN32)
#include <windows.h>
#elif defined(__SWITCH__)
#include <switch.h>
#else
#include <sys/mman.h>
#endif
#endif

/* ---- the guest space ---- */

#ifdef BOZ_GUEST_BASE
/* Guest layout inside the reservation: pages (stacks, the emulator's code page, generated stubs)
 * from GUEST_LOWEST_ADDRESS, the game image at its own base (0x4a000000), the heap above. */
enum {
    PAGES_FIRST = GUEST_LOWEST_ADDRESS,
    PAGES_END = 0x4a000000u,
    HEAP_FIRST = 0x50000000u,
};

uint8_t *g_guest_base;
static pthread_mutex_t g_space_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_pages_next = PAGES_FIRST;

bool guest_space_init(void) {
    pthread_mutex_lock(&g_space_lock);
    if (!g_guest_base) {
#if defined(_WIN32)
        /* Top down: far above 4 GiB, as on Android, so nothing works only by accident. */
        void *base = VirtualAlloc(NULL, (size_t)1 << 32, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
#elif defined(__SWITCH__)
        void *base = NULL; /* the Switch port reserves its space in platform code */
#else
        void *base = mmap(NULL, (size_t)1 << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                          -1, 0);
        if (base == MAP_FAILED) {
            base = NULL;
        }
#endif
        if (!base) {
            pthread_mutex_unlock(&g_space_lock);
            fprintf(stderr, "[memory] unable to reserve the 4 GiB guest space\n");
            return false;
        }
        g_guest_base = base;
        fprintf(stderr, "[memory] guest space at %p\n", base);
    }
    pthread_mutex_unlock(&g_space_lock);
    return true;
}

/* Commits [address, address + size) of the guest space. */
static void *commit(uint32_t address, size_t size) {
    uint8_t *host = g_guest_base + address;
#if defined(_WIN32)
    if (!VirtualAlloc(host, size, MEM_COMMIT, PAGE_READWRITE)) {
        return NULL;
    }
#elif defined(__SWITCH__)
    (void)size;
#else
    if (mprotect(host, size, PROT_READ | PROT_WRITE) != 0) {
        return NULL;
    }
#endif
    return host;
}

void *guest_pages_alloc(uint32_t want, size_t size) {
    if (!guest_space_init()) {
        return NULL;
    }
    size = (size + 0xffffu) & ~(size_t)0xffffu;
    pthread_mutex_lock(&g_space_lock);
    uint32_t address = want;
    if (!address) {
        if (size > PAGES_END - g_pages_next) {
            pthread_mutex_unlock(&g_space_lock);
            fprintf(stderr, "[memory] guest page area exhausted (%zu bytes)\n", size);
            return NULL;
        }
        address = g_pages_next;
        g_pages_next += (uint32_t)size;
    }
    void *host = commit(address, size);
    pthread_mutex_unlock(&g_space_lock);
    if (host) {
        memset(host, 0, size);
    }
    return host;
}

void guest_pages_free(void *pages, size_t size) {
    (void)pages; /* bump allocated: stays reserved */
    (void)size;
}

uint32_t guest_address_checked(const void *ptr, const char *file, int line) {
    uintptr_t value = (uintptr_t)ptr;
    if (value < GUEST_LOWEST_ADDRESS) {
        return (uint32_t)value;
    }
    uintptr_t offset = value - (uintptr_t)g_guest_base;
    if (value < (uintptr_t)g_guest_base || offset > 0xffffffffu || offset < GUEST_LOWEST_ADDRESS) {
        static const char *reported[32];
        for (int i = 0; i < 32; ++i) {
            if (reported[i] == file) {
                break;
            }
            if (!reported[i]) {
                reported[i] = file;
                fprintf(stderr, "[memory] %s:%d gives the game a pointer outside its space (%p)\n",
                        file, line, ptr);
                break;
            }
        }
        return 0;
    }
    return (uint32_t)offset;
}

#else /* identity map: the guest address is the host address */

void *guest_pages_alloc(uint32_t want, size_t size) {
    return plat_alloc(want ? (void *)(uintptr_t)want : NULL, size, true);
}

void guest_pages_free(void *pages, size_t size) {
    plat_free(pages, size);
}

#if UINTPTR_MAX > 0xffffffffu
uint32_t guest_address_checked(const void *ptr, const char *file, int line) {
    uintptr_t value = (uintptr_t)ptr;
    if (value > 0xffffffffu) {
        static const char *reported[32];
        for (int i = 0; i < 32; ++i) {
            if (reported[i] == file) {
                break;
            }
            if (!reported[i]) {
                reported[i] = file;
                fprintf(stderr, "[memory] %s:%d gives the game a pointer above 4 GiB (%p)\n", file,
                        line, ptr);
                break;
            }
        }
    }
    return (uint32_t)value;
}
#endif
#endif

/* Programs without the emulator (unit tests) call host functions natively: the guest address of
 * a function is its own address. The emulator's definition replaces this one. */
__attribute__((weak)) uint32_t arm_emu_guest_fn_sig(const void *fn, const char *signature) {
    (void)signature;
    return (uint32_t)(uintptr_t)fn;
}

/* ---- strings host functions return ---- */

static bool game_can_see(const void *ptr) {
    uintptr_t value = (uintptr_t)ptr;
#ifdef BOZ_GUEST_BASE
    uintptr_t base = (uintptr_t)g_guest_base;
    return value >= base + GUEST_LOWEST_ADDRESS && value - base <= 0xffffffffu;
#else
    return value <= 0xffffffffu;
#endif
}

uint32_t guest_string_address(const char *text) {
    if (!text || game_can_see(text)) {
        return GUEST_ADDRESS(text);
    }
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char *copies[512];
    static size_t count;
    pthread_mutex_lock(&lock);
    char *copy = NULL;
    for (size_t i = 0; i < count && !copy; ++i) {
        if (strcmp(copies[i], text) == 0) {
            copy = copies[i];
        }
    }
    if (!copy) {
        copy = guest_strdup(text);
        if (copy && count < sizeof(copies) / sizeof(copies[0])) {
            copies[count++] = copy;
        }
    }
    pthread_mutex_unlock(&lock);
    return copy ? GUEST_ADDRESS(copy) : 0;
}

/* ---- scratch space for callback data ---- */

enum { SCRATCH_SIZE = 256 * 1024 };

static _Thread_local struct {
    uint8_t *base;
    size_t used;
} t_scratch;

void *guest_scratch_push(size_t size) {
    if (!t_scratch.base) {
        t_scratch.base = guest_malloc(SCRATCH_SIZE);
        if (!t_scratch.base) {
            fprintf(stderr, "[memory] no guest scratch space\n");
            abort();
        }
    }
    size_t start = (t_scratch.used + 15u) & ~(size_t)15u;
    if (size > SCRATCH_SIZE - start) {
        fprintf(stderr, "[memory] guest scratch space exhausted (%zu bytes)\n", size);
        abort();
    }
    t_scratch.used = start + size;
    memset(t_scratch.base + start, 0, size);
    return t_scratch.base + start;
}

void guest_scratch_pop(void *block) {
    uint8_t *at = block;
    if (at && t_scratch.base && at >= t_scratch.base && at <= t_scratch.base + t_scratch.used) {
        t_scratch.used = (size_t)(at - t_scratch.base);
    }
}

uint32_t guest_block_in(const void *host, size_t size) {
#if UINTPTR_MAX > 0xffffffffu
    if (!host) {
        return 0;
    }
    void *copy = guest_scratch_push(size);
    memcpy(copy, host, size);
    return GUEST_ADDRESS(copy);
#else
    (void)size;
    return (uint32_t)(uintptr_t)host;
#endif
}

void guest_block_out(void *host, uint32_t guest, size_t size) {
#if UINTPTR_MAX > 0xffffffffu
    if (!host || !guest) {
        return;
    }
    void *copy = GUEST_POINTER(guest);
    memcpy(host, copy, size);
    guest_scratch_pop(copy);
#else
    (void)host;
    (void)guest;
    (void)size;
#endif
}

/* ---- the guest heap (64-bit hosts) ---- */

#if BOZ_GUEST_HEAP

enum {
    HEAP_ALIGN = 16,
    HEAP_HEADER = 16,
    HEAP_MIN_BLOCK = 32,
    HEAP_BINS = 64,
    FLAG_USED = 1u,
    FLAG_PREV_USED = 2u,
};

/* What is left of the low 4 GiB around the game image has room for this; a Call of the Dead
 * match peaks around 155 MB of guest heap. */
#define HEAP_SIZE ((size_t)768 << 20)

struct block {
    uint32_t prev_size; /* valid only when the previous block is free */
    uint32_t size;      /* block size including header, low bits are flags */
    uint32_t next_free;
    uint32_t prev_free;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_bins[HEAP_BINS];
static uint32_t g_base, g_top, g_limit; /* guest addresses */
static size_t g_used;
/* Everything from g_zero up to g_limit is zero (never touched, or given back with plat_discard):
 * calloc only clears what lies below it. g_zero >= g_top. */
static uint32_t g_zero;
/* Free blocks this large give their inner pages back to the system, so the heap does not stay at
 * its high-water mark (level loads); smaller ones are reused too often to pay a system call. */
#define DISCARD_MIN ((uint32_t)1 << 20)

static struct block *B(uint32_t addr) {
    return (struct block *)GUEST_POINTER(addr);
}

static uint32_t bsize(uint32_t addr) {
    return B(addr)->size & ~15u;
}

static int bin_index(uint32_t size) {
    if (size <= 512) {
        return (int)(size / 16u) - 2;
    }
    int log2 = 31 - __builtin_clz(size);
    int bin = 30 + (log2 - 9) * 2 + (int)((size >> (log2 - 1)) & 1u);
    return bin < HEAP_BINS ? bin : HEAP_BINS - 1;
}

static void unlink_free(uint32_t addr) {
    struct block *b = B(addr);
    if (b->prev_free) {
        B(b->prev_free)->next_free = b->next_free;
    } else {
        g_bins[bin_index(bsize(addr))] = b->next_free;
    }
    if (b->next_free) {
        B(b->next_free)->prev_free = b->prev_free;
    }
}

static void link_free(uint32_t addr) {
    uint32_t size = bsize(addr);
    int bin = bin_index(size);
    struct block *b = B(addr);
    b->prev_free = 0;
    b->next_free = g_bins[bin];
    if (g_bins[bin]) {
        B(g_bins[bin])->prev_free = addr;
    }
    g_bins[bin] = addr;
    uint32_t next = addr + size;
    if (next < g_top) {
        B(next)->prev_size = size;
        B(next)->size &= ~FLAG_PREV_USED;
    }
}

static void set_next_prev_used(uint32_t addr) {
    uint32_t next = addr + bsize(addr);
    if (next < g_top) {
        B(next)->size |= FLAG_PREV_USED;
    }
}

static void heap_ready(void) {
    if (g_base) {
        return;
    }
#ifdef BOZ_GUEST_BASE
    void *region = guest_pages_alloc(HEAP_FIRST, HEAP_SIZE);
#else
    void *region = plat_alloc(NULL, HEAP_SIZE, false);
    if (region && (uintptr_t)region + HEAP_SIZE > ((uint64_t)1 << 32)) {
        region = NULL;
    }
#endif
    if (!region) {
        fprintf(stderr, "[memory] unable to reserve the %zu MB guest heap\n", HEAP_SIZE >> 20);
        abort();
    }
    g_base = GUEST_ADDRESS(region);
    g_top = g_base + 0x1000u; /* a guard gap: no block at the region's first byte */
    g_limit = g_base + (uint32_t)HEAP_SIZE;
    g_zero = g_top;
}

static uint32_t page_up(uint32_t addr) {
    return (addr + 0xfffu) & ~0xfffu;
}

static uint32_t page_down(uint32_t addr) {
    return addr & ~0xfffu;
}

/* Gives the whole pages inside [lo, hi) back to the system. */
static void discard_range(uint32_t lo, uint32_t hi) {
    lo = page_up(lo);
    hi = page_down(hi);
    if (hi > lo) {
        plat_discard(GUEST_POINTER(lo), hi - lo);
    }
}

/* g_top moved up to top: what lies above it is still zero. */
static void grew_to(uint32_t top) {
    if (top > g_zero) {
        g_zero = top;
    }
}

/* The tail from addr up was freed: it becomes zero again (whole pages; the partial first page
 * stays below g_zero). */
static void shrink_to(uint32_t addr) {
    uint32_t old_top = g_top;
    g_top = addr;
    discard_range(addr, page_up(old_top));
    g_zero = page_up(addr) > g_top ? page_up(addr) : g_top;
}

static uint32_t take_from_top(uint32_t need) {
    if (need > g_limit - g_top) {
        return 0;
    }
    uint32_t addr = g_top;
    g_top += need;
    grew_to(g_top);
    B(addr)->size = need | FLAG_USED | FLAG_PREV_USED;
    return addr;
}

static void split(uint32_t addr, uint32_t need) {
    uint32_t size = bsize(addr);
    if (size - need >= HEAP_MIN_BLOCK) {
        uint32_t flags = B(addr)->size & 15u;
        B(addr)->size = need | flags;
        uint32_t rest = addr + need;
        B(rest)->size = (size - need) | FLAG_PREV_USED;
        link_free(rest);
    }
}

static uint32_t need_for(size_t size) {
    if (size > 0xffff0000u) {
        return 0;
    }
    uint32_t need = ((uint32_t)size + HEAP_HEADER + HEAP_ALIGN - 1) & ~(uint32_t)(HEAP_ALIGN - 1);
    return need < HEAP_MIN_BLOCK ? HEAP_MIN_BLOCK : need;
}

/* zero_from (optional): the payload is zero from there on (its end when nothing is known). */
static uint32_t alloc_locked(size_t size, uint32_t *zero_from) {
    uint32_t need = need_for(size);
    if (!need) {
        return 0;
    }
    for (int bin = bin_index(need); bin < HEAP_BINS; ++bin) {
        for (uint32_t addr = g_bins[bin]; addr; addr = B(addr)->next_free) {
            if (bsize(addr) >= need) {
                unlink_free(addr);
                B(addr)->size |= FLAG_USED;
                set_next_prev_used(addr);
                split(addr, need);
                g_used += bsize(addr);
                if (zero_from) {
                    *zero_from = addr + bsize(addr);
                }
                return addr + HEAP_HEADER;
            }
            if (bin < 30) {
                break; /* exact classes: the first entry decides */
            }
        }
    }
    uint32_t zero = g_zero;
    uint32_t addr = take_from_top(need);
    if (!addr) {
        fprintf(stderr, "[memory] guest heap exhausted (%zu bytes requested)\n", size);
        return 0;
    }
    g_used += need;
    if (zero_from) {
        *zero_from = zero > addr + HEAP_HEADER ? zero : addr + HEAP_HEADER;
    }
    return addr + HEAP_HEADER;
}

static bool owned(uint32_t addr) {
    return addr >= g_base && addr < g_top && (B(addr)->size & FLAG_USED);
}

static void free_locked(uint32_t ptr) {
    uint32_t addr = ptr - HEAP_HEADER;
    if (!owned(addr)) {
        static int reported;
        if (reported++ < 8) {
            fprintf(stderr, "[memory] ignoring free of 0x%08x: not a guest heap block\n", ptr);
        }
        return;
    }
    uint32_t size = bsize(addr);
    g_used -= size;
    uint32_t flags = B(addr)->size & FLAG_PREV_USED;
    uint32_t next = addr + size;
    if (next < g_top && !(B(next)->size & FLAG_USED)) {
        unlink_free(next);
        size += bsize(next);
    }
    if (!(flags & FLAG_PREV_USED)) {
        uint32_t prev = addr - B(addr)->prev_size;
        unlink_free(prev);
        size += bsize(prev);
        addr = prev;
        flags = B(prev)->size & FLAG_PREV_USED;
    }
    if (addr + size == g_top) {
        shrink_to(addr); /* give the tail back */
        return;
    }
    B(addr)->size = size | flags;
    link_free(addr);
    if (size >= DISCARD_MIN) {
        discard_range(addr + HEAP_HEADER, addr + size); /* keeps the header and its free-list links */
    }
}

void *guest_malloc(size_t size) {
    pthread_mutex_lock(&g_lock);
    heap_ready();
    uint32_t p = alloc_locked(size, NULL);
    pthread_mutex_unlock(&g_lock);
    return p ? GUEST_POINTER(p) : NULL;
}

void *guest_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return NULL;
    }
    size_t bytes = count * size;
    pthread_mutex_lock(&g_lock);
    heap_ready();
    uint32_t zero_from = 0;
    uint32_t p = alloc_locked(bytes, &zero_from);
    pthread_mutex_unlock(&g_lock);
    if (!p) {
        return NULL;
    }
    /* Only what may hold old data: fresh memory from the top is already zero. */
    size_t dirty = zero_from > p ? zero_from - p : 0;
    memset(GUEST_POINTER(p), 0, dirty < bytes ? dirty : bytes);
    return GUEST_POINTER(p);
}

void guest_free(void *ptr) {
    if (!ptr) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    free_locked(GUEST_ADDRESS(ptr));
    pthread_mutex_unlock(&g_lock);
}

void *guest_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return guest_malloc(size);
    }
    uint32_t need = need_for(size);
    if (!need) {
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    uint32_t p = GUEST_ADDRESS(ptr);
    uint32_t addr = p - HEAP_HEADER;
    if (!owned(addr)) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[memory] realloc of 0x%08x: not a guest heap block\n", p);
        return NULL;
    }
    uint32_t have = bsize(addr);
    if (need <= have) {
        pthread_mutex_unlock(&g_lock);
        return ptr;
    }
    uint32_t next = addr + have;
    if (next == g_top && need - have <= g_limit - g_top) {
        g_top = addr + need;
        grew_to(g_top);
        B(addr)->size = need | (B(addr)->size & 15u);
        g_used += need - have;
        pthread_mutex_unlock(&g_lock);
        return ptr;
    }
    if (next < g_top && !(B(next)->size & FLAG_USED) && have + bsize(next) >= need) {
        unlink_free(next);
        B(addr)->size += bsize(next);
        set_next_prev_used(addr);
        split(addr, need);
        g_used += bsize(addr) - have;
        pthread_mutex_unlock(&g_lock);
        return ptr;
    }
    uint32_t fresh = alloc_locked(size, NULL);
    if (fresh) {
        memcpy(GUEST_POINTER(fresh), ptr, have - HEAP_HEADER);
        free_locked(p);
    }
    pthread_mutex_unlock(&g_lock);
    return fresh ? GUEST_POINTER(fresh) : NULL;
}

char *guest_strdup(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = guest_malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

void guest_heap_stats(size_t *capacity, size_t *used) {
    pthread_mutex_lock(&g_lock);
    *capacity = g_base ? HEAP_SIZE : 0;
    *used = g_used;
    pthread_mutex_unlock(&g_lock);
}
#endif
