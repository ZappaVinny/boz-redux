/* The guest heap (src/guest_heap.c on 64-bit hosts; the C library's malloc on 32-bit ones):
 * calloc always gives zeroed memory, whatever the block held before, and large freed blocks give
 * their pages back to the system. */
#include "guest_memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

static int all_zero(const unsigned char *p, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (p[i]) {
            return 0;
        }
    }
    return 1;
}

static void test_calloc_is_zero_after_reuse(void) {
    /* A dirty block in the middle (a small block after it keeps it off the top): reused as is. */
    unsigned char *middle = guest_malloc(3 << 20);
    unsigned char *keep = guest_malloc(64);
    assert(middle && keep);
    memset(middle, 0xaa, 3 << 20);
    guest_free(middle);
    unsigned char *again = guest_calloc(1, 3 << 20);
    assert(again && all_zero(again, 3 << 20));

    /* A dirty block at the top, freed (the top shrinks) and taken again from the top. */
    unsigned char *top = guest_malloc(5 << 20);
    assert(top);
    memset(top, 0x55, 5 << 20);
    guest_free(top);
    unsigned char *fresh = guest_calloc(5 << 20, 1);
    assert(fresh && all_zero(fresh, 5 << 20));

    /* Small blocks with an odd size: the dirty partial page below the zero mark is cleared too. */
    for (int i = 0; i < 64; ++i) {
        unsigned char *small = guest_malloc(1000 + (size_t)i * 37);
        memset(small, 0xff, 1000 + (size_t)i * 37);
        guest_free(small);
        unsigned char *zeroed = guest_calloc(1, 1000 + (size_t)i * 37);
        assert(all_zero(zeroed, 1000 + (size_t)i * 37));
        guest_free(zeroed);
    }
    guest_free(fresh);
    guest_free(again);
    guest_free(keep);
}

static void test_realloc_keeps_contents(void) {
    unsigned char *p = guest_malloc(100);
    memset(p, 7, 100);
    p = guest_realloc(p, 2 << 20);
    assert(p);
    for (int i = 0; i < 100; ++i) {
        assert(p[i] == 7);
    }
    guest_free(p);
}

#if defined(__linux__) && BOZ_GUEST_HEAP
/* Resident pages of [p, p + size), from mincore. */
static size_t resident_pages(const void *p, size_t size) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t lo = (uintptr_t)p & ~(page - 1);
    size_t pages = ((uintptr_t)p + size - lo + page - 1) / page;
    static unsigned char vec[(64 << 20) / 4096];
    assert(pages <= sizeof(vec));
    assert(mincore((void *)lo, pages * page, vec) == 0);
    size_t count = 0;
    for (size_t i = 0; i < pages; ++i) {
        count += vec[i] & 1u;
    }
    return count;
}

static void test_large_free_gives_pages_back(void) {
    size_t size = 32 << 20;
    unsigned char *big = guest_malloc(size);
    unsigned char *keep = guest_malloc(64); /* keeps big off the top: the free-list path */
    memset(big, 1, size);
    assert(resident_pages(big, size) > (size / 4096) * 9 / 10);
    guest_free(big);
    assert(resident_pages(big, size) < 4);
    guest_free(keep);
}
#endif

int main(void) {
    test_calloc_is_zero_after_reuse();
    test_realloc_keeps_contents();
#if defined(__linux__) && BOZ_GUEST_HEAP
    test_large_free_gives_pages_back();
#endif
    printf("guest_heap_test: all passed\n");
    return 0;
}
