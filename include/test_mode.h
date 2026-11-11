/* BOZ Test: --test FILE runs a test script in a mod's Lua environment while the game plays
 * (usually headless), then exits with its result. The SDK's `bozkit test` starts the client this
 * way and collects <out>/result.json, the log and the screenshots into a report. */
#ifndef BOZ_TEST_MODE_H
#define BOZ_TEST_MODE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exit codes: passed, failed, the test did not finish in time, the test could not start. */
enum { TEST_EXIT_PASSED = 0, TEST_EXIT_FAILED = 1, TEST_EXIT_TIMEOUT = 3, TEST_EXIT_ERROR = 4 };

struct test_options {
    const char *file;    /* the test script */
    const char *mod;     /* the mod whose environment it runs in (NULL: the one holding it) */
    const char *out;     /* where result.json and screenshots go (NULL: next to the test) */
    const char *role;    /* co-op tests: "host" or "guest" (NULL: "solo") */
    int timeout_seconds; /* 0: 300 */
};

/* Switches test mode on and starts the timeout watchdog. */
void test_mode_start(const struct test_options *options);
bool test_mode_active(void);
const struct test_options *test_mode_options(void);

/* A Lua error in a mod (counted into the result). */
void test_mode_note_error(const char *mod_id, const char *message);
int test_mode_error_count(void);

/* Writes <out>/result.json (json: the test's own result object, or NULL) and exits. */
void test_mode_finish(int exit_code, const char *json);

/* Asks for the next presented frame as <out>/<name>.ppm; true once a request is pending. */
bool test_mode_request_screenshot(const char *name);
/* The path for the pending screenshot (and clears the request), or NULL. */
const char *test_mode_take_screenshot(void);

#ifdef __cplusplus
}
#endif

#endif
