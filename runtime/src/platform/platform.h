#ifndef BOZ_PLATFORM_H
#define BOZ_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool plat_init(void);
/* Windows: installs the crash logger again (other libraries may replace it); no-op elsewhere. */
void plat_reinstall_crash_handler(void);
void plat_sleep_us(uint64_t microseconds);

void *plat_lib_open(const char *name);
void *plat_lib_symbol(void *library, const char *name);
int plat_lib_close(void *library);
const char *plat_lib_error(void);
bool plat_lib_path(const void *address, char *out, size_t out_size);

bool plat_address_in_module(const void *address);
const char *plat_symbol_name(const void *address);

void *plat_alloc(void *want, size_t size, bool executable);
void plat_free(void *address, size_t size);
size_t plat_page_size(void);
bool plat_region(uintptr_t address, uint64_t *start, uint64_t *end);
/* A range around address that is safe to identity-map for guest data access: never part of a
   loaded module image, so guest jumps into host code keep trapping. */
bool plat_data_window(uintptr_t address, uint64_t *start, uint64_t *end);

#include <stdio.h>
FILE *plat_memfile(const void *buffer, size_t size);
bool plat_random_bytes(void *buffer, size_t size);
int plat_rename_replace(const char *from, const char *to);

#endif
