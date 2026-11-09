#ifndef BOZ_GUEST_MEMORY_H
#define BOZ_GUEST_MEMORY_H

/* Memory the game can see. The game addresses memory with 32-bit pointers, so on a 64-bit host
 * everything it reads or writes has to sit below 4 GiB: guest_malloc and friends allocate from a
 * heap there (src/guest_heap.c). On 32-bit hosts every address qualifies and they are the C
 * library's own functions.
 *
 * Use them for every block whose address the game receives: its own heap (s3eMalloc), handles
 * and buffers the s3e functions return, data passed to its callbacks. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* A host pointer as the 32-bit address the game uses. On 64-bit hosts a pointer above 4 GiB is a
 * bug (memory that did not come from the functions above): it is logged once per source file. */
#if UINTPTR_MAX > 0xffffffffu
uint32_t guest_address_checked(const void *ptr, const char *file, int line);
#define GUEST_ADDRESS(ptr) guest_address_checked((const void *)(ptr), __FILE__, __LINE__)
#else
#define GUEST_ADDRESS(ptr) ((uint32_t)(uintptr_t)(ptr))
#endif

#endif
