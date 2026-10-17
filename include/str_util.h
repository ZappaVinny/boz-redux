#ifndef BOZ_STR_UTIL_H
#define BOZ_STR_UTIL_H

#include <stddef.h>

/* Strips leading and trailing whitespace in place; returns the start of the text. */
char *str_trim(char *text);
/* The part of a path after its last '/' or '\\' (the whole text without one; "" for NULL). */
const char *str_base_name(const char *path);
/* Lower-case copy, truncated to out_size. */
void str_lower(char *out, size_t out_size, const char *in);
/* Lower-case copy with '\\' turned into '/', for comparing paths. */
void str_lower_path(char *out, size_t out_size, const char *in);

#endif
