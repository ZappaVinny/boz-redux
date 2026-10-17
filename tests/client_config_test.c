#include "client_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(text, file);
    fclose(file);
}

static void expect_env(const char *name, const char *value) {
    const char *actual = getenv(name);
    if (!value) {
        assert(!actual);
    } else {
        assert(actual && strcmp(actual, value) == 0);
    }
}

int main(void) {
    char root[] = "/tmp/client_config_test.XXXXXX";
    assert(mkdtemp(root));
    char path[256];
    snprintf(path, sizeof(path), "%s/client.ini", root);

    /* First run writes the default file, whose values match the built-in defaults. */
    unsetenv("BOZ_WINDOWED");
    unsetenv("BOZ_VSYNC");
    unsetenv("BOZ_FPS");
    unsetenv("BOZ_MOUSE_SENS");
    unsetenv("BOZ_TRACE_STATUS");
    client_config_load(root);
    assert(access(path, R_OK) == 0);
    expect_env("BOZ_WINDOWED", "0");
    expect_env("BOZ_VSYNC", "1");
    expect_env("BOZ_FPS", NULL);
    expect_env("BOZ_MOUSE_SENS", "12000");
    expect_env("BOZ_TRACE_STATUS", NULL);

    /* File values apply; variables already set by the user win; comments and bad lines are
     * skipped. */
    unsetenv("BOZ_WINDOWED");
    unsetenv("BOZ_VSYNC");
    unsetenv("BOZ_DISPLAY");
    unsetenv("BOZ_STRETCH");
    unsetenv("BOZ_LOOK_MODE");
    setenv("BOZ_MOUSE_SENS", "500", 1);
    write_file(path, "[display]\n"
                     "fullscreen = false  # windowed\n"
                     "vsync = adaptive\n"
                     "fps_limit = 144\n"
                     "resolution = 1920x1080\n"
                     "scaling = stretch\n"
                     "nonsense\n"
                     "[input]\n"
                     "mouse_sensitivity = 30000\n"
                     "look_mode = swipe\n"
                     "look_mode_typo = x\n"
                     "[keys]\n"
                     "crouch = C, Left Ctrl\n"
                     "melee =\n"
                     "[debug]\n"
                     "status = true\n");
    client_config_load(root);
    expect_env("BOZ_WINDOWED", "1");
    expect_env("BOZ_VSYNC", "-1");
    expect_env("BOZ_FPS", "144");
    expect_env("BOZ_DISPLAY", "1920x1080");
    expect_env("BOZ_STRETCH", "1");
    expect_env("BOZ_MOUSE_SENS", "500");
    expect_env("BOZ_LOOK_MODE", "swipe");
    expect_env("BOZ_TRACE_STATUS", "1");
    assert(strcmp(client_config_key_binding("crouch"), "C, Left Ctrl") == 0);
    assert(strcmp(client_config_key_binding("melee"), "") == 0);
    assert(strcmp(client_config_key_binding("shoot"), "Mouse1") == 0); /* from the first load */
    assert(client_config_key_binding("jump") == NULL);

    unlink(path);
    rmdir(root);
    puts("ok client_config");
    return 0;
}
