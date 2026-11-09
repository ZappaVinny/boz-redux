/* The guest heap for 64-bit hosts (see include/guest_memory.h): a boundary-tag allocator with
 * segregated free lists over one region below 4 GiB. Payloads are
 * 16-byte aligned, as the game's allocator expects. */
#include "guest_memory.h"

#include <stdio.h>

/* ---- scratch space for callback data (both 32- and 64-bit hosts) ---- */

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
    return (uint32_t)(uintptr_t)copy;
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
    void *copy = (void *)(uintptr_t)guest;
    memcpy(host, copy, size);
    guest_scratch_pop(copy);
#else
    (void)host;
    (void)guest;
    (void)size;
#endif
}

#if BOZ_GUEST_HEAP
#include "platform/platform.h"

#include <pthread.h>
#include <stdio.h>

enum {
    ALIGN = 16,
    HDR = 16,
    MIN_BLOCK = 32,
    BIN_COUNT = 64,
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
static uint32_t g_bins[BIN_COUNT];
static uint32_t g_base, g_top, g_limit;
static size_t g_used;

static struct block *B(uint32_t addr) {
    return (struct block *)(uintptr_t)addr;
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
    return bin < BIN_COUNT ? bin : BIN_COUNT - 1;
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
    void *region = plat_alloc(NULL, HEAP_SIZE, false);
    if (!region || (uintptr_t)region + HEAP_SIZE > ((uint64_t)1 << 32)) {
        fprintf(stderr, "[memory] unable to reserve the %zu MB guest heap below 4 GiB\n",
                HEAP_SIZE >> 20);
        abort();
    }
    g_base = (uint32_t)(uintptr_t)region;
    g_top = g_base + 0x1000u; /* a guard gap: no block at the region's first byte */
    g_limit = g_base + (uint32_t)HEAP_SIZE;
}

static uint32_t take_from_top(uint32_t need) {
    if (need > g_limit - g_top) {
        return 0;
    }
    uint32_t addr = g_top;
    g_top += need;
    B(addr)->size = need | FLAG_USED | FLAG_PREV_USED;
    return addr;
}

static void split(uint32_t addr, uint32_t need) {
    uint32_t size = bsize(addr);
    if (size - need >= MIN_BLOCK) {
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
    uint32_t need = ((uint32_t)size + HDR + ALIGN - 1) & ~(uint32_t)(ALIGN - 1);
    return need < MIN_BLOCK ? MIN_BLOCK : need;
}

static uint32_t alloc_locked(size_t size) {
    uint32_t need = need_for(size);
    if (!need) {
        return 0;
    }
    for (int bin = bin_index(need); bin < BIN_COUNT; ++bin) {
        for (uint32_t addr = g_bins[bin]; addr; addr = B(addr)->next_free) {
            if (bsize(addr) >= need) {
                unlink_free(addr);
                B(addr)->size |= FLAG_USED;
                set_next_prev_used(addr);
                split(addr, need);
                g_used += bsize(addr);
                return addr + HDR;
            }
            if (bin < 30) {
                break; /* exact classes: the first entry decides */
            }
        }
    }
    uint32_t addr = take_from_top(need);
    if (!addr) {
        fprintf(stderr, "[memory] guest heap exhausted (%zu bytes requested)\n", size);
        return 0;
    }
    g_used += need;
    return addr + HDR;
}

static bool owned(uint32_t addr) {
    return addr >= g_base && addr < g_top && (B(addr)->size & FLAG_USED);
}

static void free_locked(uint32_t ptr) {
    uint32_t addr = ptr - HDR;
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
        g_top = addr; /* give the tail back */
        return;
    }
    B(addr)->size = size | flags;
    link_free(addr);
}

void *guest_malloc(size_t size) {
    pthread_mutex_lock(&g_lock);
    heap_ready();
    uint32_t p = alloc_locked(size);
    pthread_mutex_unlock(&g_lock);
    return (void *)(uintptr_t)p;
}

void *guest_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return NULL;
    }
    void *p = guest_malloc(count * size);
    if (p) {
        memset(p, 0, count * size);
    }
    return p;
}

void guest_free(void *ptr) {
    if (!ptr) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    free_locked((uint32_t)(uintptr_t)ptr);
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
    uint32_t p = (uint32_t)(uintptr_t)ptr;
    uint32_t addr = p - HDR;
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
    uint32_t fresh = alloc_locked(size);
    if (fresh) {
        memcpy((void *)(uintptr_t)fresh, ptr, have - HDR);
        free_locked(p);
    }
    pthread_mutex_unlock(&g_lock);
    return (void *)(uintptr_t)fresh;
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
