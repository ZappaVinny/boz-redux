/* Helpers shared by the tests (tests/test_support.c). */
#ifndef BOZ_TEST_SUPPORT_H
#define BOZ_TEST_SUPPORT_H

#include <stddef.h>

/* Makes a new folder in the system's temporary folder (TEMP/TMP on Windows, TMPDIR or /tmp
 * elsewhere), named <name>.XXXXXX; returns out, or NULL. */
char *test_temp_dir(char *out, size_t size, const char *name);
/* Deletes a folder and everything in it. */
void test_remove_tree(const char *path);

#endif
