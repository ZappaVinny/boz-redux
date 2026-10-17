#include "client_config.h"
#include "mod_runtime.h"
#include "mods.h"
#include "native_input.h"
#include "game_patches.h"
#include "gamedef.h"
#include "str_util.h"
#include "s3e_host.h"
#include "s3e_host_internal.h"
#include "s3e_image.h"
#include "platform/platform.h"
#if defined(_WIN32)
#include "platform/gl_probe.h"
#endif

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_emu.h"


/* The game definition (gamedef/): next to the game data, else next to the executable. */
static void load_game_definition(const char *root) {
    char exe[1024] = "";
    if (plat_lib_path((const void *)(uintptr_t)&load_game_definition, exe, sizeof(exe))) {
        char *name = (char *)str_base_name(exe);
        if (name > exe) {
            name[-1] = '\0';
        }
    }
    gamedef_load(root, exe[0] ? exe : NULL);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--run] [--root DIR] [--display-size WIDTHxHEIGHT] "
            "IMAGE.s3e.unpacked\n",
            argv0);
}

static bool parse_display_size(const char *value, uint32_t *width, uint32_t *height) {
    char *width_end = NULL;
    char *height_end = NULL;
    unsigned long parsed_width = strtoul(value, &width_end, 10);
    if (width_end == value || (*width_end != 'x' && *width_end != 'X')) {
        return false;
    }
    unsigned long parsed_height = strtoul(width_end + 1, &height_end, 10);
    if (height_end == width_end + 1 || *height_end != '\0' || !parsed_width || !parsed_height ||
        parsed_width > UINT16_MAX || parsed_height > UINT16_MAX) {
        return false;
    }
    *width = (uint32_t)parsed_width;
    *height = (uint32_t)parsed_height;
    return true;
}

static void terminate_handler(int sig) {
    (void)sig;
    _Exit(0);
}

static void install_terminate_handlers(void) {
#if defined(_WIN32)
    signal(SIGINT, terminate_handler);
    signal(SIGTERM, terminate_handler);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = terminate_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
#endif
}

int main(int argc, char **argv) {
#if defined(_WIN32)
    if (argc >= 2 && strcmp(argv[1], "--gl-probe") == 0) {
        return gl_probe_run(argc >= 3 ? argv[2] : NULL);
    }
#endif
    bool run = false;
    const char *root = NULL;
    const char *image_path = NULL;
    uint32_t display_width = 1280;
    uint32_t display_height = 720;
    bool display_size_given = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--run") == 0) {
            run = true;
        } else if (strcmp(argv[i], "--root") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            root = argv[++i];
        } else if (strcmp(argv[i], "--display-size") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            display_size_given = true;
            if (!parse_display_size(argv[++i], &display_width, &display_height)) {
                fprintf(stderr, "invalid display size: %s\n", argv[i]);
                return 2;
            }
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!image_path) {
            image_path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!image_path) {
        usage(argv[0]);
        return 2;
    }

    client_config_load(root);
    mods_init(root);
    /* Render resolution: --display-size, else BOZ_DISPLAY / client.ini, else 1280x720. */
    const char *display_setting = getenv("BOZ_DISPLAY");
    if (!display_size_given && display_setting &&
        !parse_display_size(display_setting, &display_width, &display_height)) {
        fprintf(stderr, "invalid render resolution: %s (expected WIDTHxHEIGHT)\n", display_setting);
        return 2;
    }

    if (!s3e_host_set_display_size(display_width, display_height)) {
        fprintf(stderr, "unable to allocate display surface: %ux%u\n", display_width,
                display_height);
        return 1;
    }

    if (!plat_init()) {
        fprintf(stderr, "platform initialisation failed\n");
        return 1;
    }
    install_terminate_handlers();
#if defined(_WIN32)
    gl_probe_select_driver();
#endif

    struct s3e_image image;
    if (!s3e_image_load(image_path, &image)) {
        return 1;
    }
    if (!s3e_image_parse_symbols(&image)) {
        s3e_image_free(&image);
        return 1;
    }

    fprintf(stderr, "S3E version=0x%x arch=0x%x symbols=%zu code=0x%x mem=0x%x\n",
            image.header.version, image.header.arch, image.symbols.count,
            image.header.code_file_size, image.header.code_mem_size);

    if (!s3e_host_init(root)) {
        s3e_image_free(&image);
        return 1;
    }
    load_game_definition(root);
    s3e_host_set_config(image.file_data + image.header.config_offset, image.header.config_size);

    struct s3e_loaded_image loaded;
    if (!s3e_image_map_and_relocate(&image, s3e_host_resolve, &loaded)) {
        s3e_host_shutdown();
        s3e_image_free(&image);
        return 1;
    }
    game_set_image_base((uintptr_t)loaded.base);
    if (!frame_interpolation_install()) {
        fprintf(stderr, "frame interpolation not installed: the game executable does not match\n");
    }

    uintptr_t player_name_address = (uintptr_t)s3e_host_player_name();
    if (player_name_address > UINT32_MAX ||
        !player_name_install(&loaded, (uint32_t)player_name_address)) {
        fprintf(stderr, "unable to apply configured player name\n");
    }

    fprintf(stderr, "mapped S3E at %p, entry=%p\n", (void *)loaded.base,
            (void *)(loaded.base + loaded.entry_offset));
    if (run) {
        if (!arm_emu_init()) {
            return 1;
        }
        arm_emu_set_image((uint32_t)(uintptr_t)loaded.base,
                          (uint32_t)((uintptr_t)loaded.base + loaded.map_size));
        arm_emu_trace_ignore((uint32_t)(uintptr_t)&s3eMallocBase);
        arm_emu_trace_ignore((uint32_t)(uintptr_t)&s3eFreeBase);
        arm_emu_trace_ignore((uint32_t)(uintptr_t)&s3eReallocBase);
        crash_recovery_install((uint32_t)(uintptr_t)loaded.base);
        /* Mods start before the game so their hooks and asset patches see everything it does. */
        lua_runtime_start();
        native_input_init();
        int rc = (int)arm_emu_call((uint32_t)(uintptr_t)(loaded.base + loaded.entry_offset), 0, NULL);
        fprintf(stderr, "S3E entry returned %d\n", rc);
    }

    s3e_loaded_image_unmap(&loaded);
    s3e_host_shutdown();
    s3e_image_free(&image);
    return 0;
}
