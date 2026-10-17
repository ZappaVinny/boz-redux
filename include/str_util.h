#ifndef BOZ_STR_UTIL_H
#define BOZ_STR_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Strips leading and trailing whitespace in place; returns the start of the text. */
char *str_trim(char *text);
/* The part of a path after its last '/' or '\\' (the whole text without one; "" for NULL). */
const char *str_base_name(const char *path);
/* Lower-case copy, truncated to out_size. */
void str_lower(char *out, size_t out_size, const char *in);
/* Lower-case copy with '\\' turned into '/', for comparing paths. */
void str_lower_path(char *out, size_t out_size, const char *in);
/* Parses "WIDTHxHEIGHT" (either x case), each side 1..65535. */
bool str_parse_size(const char *text, uint32_t *width, uint32_t *height);

#endif
