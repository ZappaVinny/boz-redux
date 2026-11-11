/* BOZ Test mode (include/test_mode.h): options, the error count, the result file, screenshot
 * requests and the watchdog that ends a test that hangs. */
#include "test_mode.h"

#include "posix_compat.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static bool g_active;
static struct test_options g_options;
static char g_out[1024];
static int g_errors;
static char g_first_error[512];
static char g_screenshot[1200];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_finished;

static void json_string(FILE *file, const char *text) {
    fputc('"', file);
    for (const unsigned char *c = (const unsigned char *)(text ? text : ""); *c; ++c) {
        if (*c == '"' || *c == '\\') {
            fprintf(file, "\\%c", *c);
        } else if (*c == '\n') {
            fputs("\\n", file);
        } else if (*c < 0x20) {
            fprintf(file, "\\u%04x", *c);
        } else {
            fputc(*c, file);
        }
    }
    fputc('"', file);
}

static void write_result(int exit_code, const char *json) {
    static const char *const STATUS[] = {"passed", "failed", "failed", "timeout", "error"};
    char path[1100];
    snprintf(path, sizeof(path), "%s/result.json", g_out);
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "[test] cannot write %s\n", path);
        return;
    }
    fputs("{\"status\": ", file);
    json_string(file, exit_code >= 0 && exit_code <= 4 ? STATUS[exit_code] : "error");
    fprintf(file, ", \"lua_errors\": %d, \"first_lua_error\": ", g_errors);
    json_string(file, g_first_error);
    fprintf(file, ", \"client\": ");
    json_string(file, BOZ_VERSION);
    fprintf(file, ", \"test\": %s}\n", json && json[0] ? json : "null");
    fclose(file);
}

static void *watchdog(void *unused) {
    (void)unused;
    int seconds = g_options.timeout_seconds > 0 ? g_options.timeout_seconds : 300;
#if defined(_WIN32)
    Sleep((DWORD)seconds * 1000u);
#else
    struct timespec wait = {seconds, 0};
    while (nanosleep(&wait, &wait) != 0) {
    }
#endif
    fprintf(stderr, "[test] timed out after %d s\n", seconds);
    test_mode_finish(TEST_EXIT_TIMEOUT, NULL);
    return NULL;
}

void test_mode_start(const struct test_options *options) {
    g_options = *options;
    g_active = true;
    if (options->out && options->out[0]) {
        snprintf(g_out, sizeof(g_out), "%s", options->out);
    } else {
        snprintf(g_out, sizeof(g_out), "%s", options->file);
        char *slash = strrchr(g_out, '/');
        char *backslash = strrchr(g_out, '\\');
        if (backslash && (!slash || backslash > slash)) {
            slash = backslash;
        }
        if (slash) {
            *slash = '\0';
        } else {
            snprintf(g_out, sizeof(g_out), ".");
        }
    }
    g_options.out = g_out;
    if (!g_options.role || !g_options.role[0]) {
        g_options.role = "solo";
    }
    fprintf(stderr, "[test] %s (%s), results in %s\n", options->file, g_options.role, g_out);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, watchdog, NULL);
    pthread_attr_destroy(&attr);
}

bool test_mode_active(void) {
    return g_active;
}

const struct test_options *test_mode_options(void) {
    return g_active ? &g_options : NULL;
}

void test_mode_note_error(const char *mod_id, const char *message) {
    if (!g_active) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_errors++) {
        snprintf(g_first_error, sizeof(g_first_error), "%s: %s", mod_id ? mod_id : "?",
                 message ? message : "");
    }
    pthread_mutex_unlock(&g_lock);
}

int test_mode_error_count(void) {
    return g_errors;
}

void test_mode_finish(int exit_code, const char *json) {
    pthread_mutex_lock(&g_lock);
    if (g_finished) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_finished = true;
    write_result(exit_code, json);
    fprintf(stderr, "[test] finished: exit %d\n", exit_code);
    fflush(stderr);
    fflush(stdout);
    /* No clean shutdown: the emulated game is mid-frame; the result is on disk. */
    _exit(exit_code);
}

bool test_mode_request_screenshot(const char *name) {
    if (!g_active || !name || !name[0] || strpbrk(name, "/\\:") || strstr(name, "..")) {
        return false;
    }
    snprintf(g_screenshot, sizeof(g_screenshot), "%s/%s.ppm", g_out, name);
    return true;
}

const char *test_mode_take_screenshot(void) {
    static char taken[sizeof(g_screenshot)];
    if (!g_screenshot[0]) {
        return NULL;
    }
    snprintf(taken, sizeof(taken), "%s", g_screenshot);
    g_screenshot[0] = '\0';
    return taken;
}
