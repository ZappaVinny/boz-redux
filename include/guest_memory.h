#ifndef BOZ_GUEST_MEMORY_H
#define BOZ_GUEST_MEMORY_H

/* Memory the game can see. The game addresses memory with 32-bit pointers ("guest addresses").
 *
 * - 32-bit hosts: a guest address is the host address, and every host address qualifies.
 * - 64-bit hosts (default): still the host address, so everything the game sees sits below
 *   4 GiB: guest_malloc and friends allocate from a heap there (src/guest_heap.c).
 * - BOZ_GUEST_BASE (Android, the Switch; on the PC for testing): the low 4 GiB of the process are
 *   not ours, so the game's memory is one 4 GiB reservation anywhere, and a guest address is an
 *   offset into it: GUEST_POINTER(address) = base + address, GUEST_ADDRESS(pointer) the reverse.
 *
 * Every block whose address the game receives comes from these functions: its own heap
 * (s3eMalloc), handles and buffers the s3e functions return, data passed to its callbacks.
 * Values below GUEST_LOWEST_ADDRESS are never memory (NULL, GL buffer offsets, small handles) and
 * pass through both conversions unchanged. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GUEST_LOWEST_ADDRESS 0x40000000u

#if UINTPTR_MAX > 0xffffffffu
#define BOZ_GUEST_HEAP 1
void *guest_malloc(size_t size);
void *guest_calloc(size_t count, size_t size);
void *guest_realloc(void *ptr, size_t size);
void guest_free(void *ptr);
char *guest_strdup(const char *text);
/* Size of the guest heap and bytes in use, for the log. */
void guest_heap_stats(size_t *capacity, size_t *used);
#else
#define BOZ_GUEST_HEAP 0
static inline void *guest_malloc(size_t size) { return malloc(size); }
static inline void *guest_calloc(size_t count, size_t size) { return calloc(count, size); }
static inline void *guest_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
static inline void guest_free(void *ptr) { free(ptr); }
static inline char *guest_strdup(const char *text) { return strdup(text); }
#endif

/* Pages the game can see (stacks, the game image, generated code), at guest address `want` when
 * it is not 0. 32-bit and default 64-bit hosts: plat_alloc below 4 GiB. Guest-base builds: pages
 * inside the reservation. Returns the host pointer. */
void *guest_pages_alloc(uint32_t want, size_t size);
void guest_pages_free(void *pages, size_t size);

#ifdef BOZ_GUEST_BASE
extern uint8_t *g_guest_base;
/* Reserves the 4 GiB guest space (idempotent). */
bool guest_space_init(void);

static inline void *GUEST_POINTER(uint32_t address) {
    return address < GUEST_LOWEST_ADDRESS ? (void *)(uintptr_t)address
                                          : (void *)(g_guest_base + address);
}
uint32_t guest_address_checked(const void *ptr, const char *file, int line);
#define GUEST_ADDRESS(ptr) guest_address_checked((const void *)(ptr), __FILE__, __LINE__)
#else
static inline bool guest_space_init(void) { return true; }
static inline void *GUEST_POINTER(uint32_t address) { return (void *)(uintptr_t)address; }
#if UINTPTR_MAX > 0xffffffffu
/* A pointer above 4 GiB is a bug (memory that did not come from the functions above): it is
 * logged once per source file. */
uint32_t guest_address_checked(const void *ptr, const char *file, int line);
#define GUEST_ADDRESS(ptr) guest_address_checked((const void *)(ptr), __FILE__, __LINE__)
#else
#define GUEST_ADDRESS(ptr) ((uint32_t)(uintptr_t)(ptr))
#endif
#endif

/* The guest address of a string a host function returns: the string itself when the game can
 * see it, else a copy in guest memory (one per distinct text, kept for the whole run). */
uint32_t guest_string_address(const char *text);

/* Scratch space in guest memory for data handed to one guest callback (its "system data"):
 * guest_scratch_push returns zeroed space, guest_scratch_pop releases it and everything pushed
 * after it. Per thread, last in first out, so nested callbacks are fine. */
void *guest_scratch_push(size_t size);
void guest_scratch_pop(void *block);

/* A host block a guest callback reads or writes: guest_block_in gives the game's address for
 * size bytes at host (a scratch copy on 64-bit hosts, the block itself on 32-bit ones), and
 * guest_block_out copies the game's changes back and releases the copy. */
uint32_t guest_block_in(const void *host, size_t size);
void guest_block_out(void *host, uint32_t guest, size_t size);

#ifdef __cplusplus
}
#endif

#endif
