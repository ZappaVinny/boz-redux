#include "s3e_host_internal.h"
#include <stdarg.h>
#if !defined(__arm__)
#include "arm_emu.h"
#endif

char g_root[1024];
void *g_egl;
void *g_gles1;
void *g_gles2;
uint8_t *g_stub_code;
size_t g_stub_code_size;
const char *g_stub_names[512];
size_t g_stub_count;
struct dtrz_index g_dtrz;
struct memory_file *g_memory_files;
struct timer_event *g_timers;
pthread_mutex_t g_timer_mutex = PTHREAD_MUTEX_INITIALIZER;
struct fbdev_window g_native_window = {640, 480};
struct surface_geometry g_surface = {0, 0, 640, 480};
uint32_t *g_surface_pixels;
struct callback_slot g_pointer_callbacks[4];
struct callback_slot g_touchpad_callbacks[8];
struct keyboard_callback_slot g_keyboard_callbacks[16];
int32_t g_pointer_x = 110;
int32_t g_pointer_y = 150;
int g_pointer_down;
uint8_t g_pointer_states[5];
int g_cursor_active = 1;
uint8_t g_is_device_resources[IS_DEVICE_RESOURCES_SIZE];
uint64_t g_host_start_us;

uint32_t s3e_guest_call(const void *fn, int argc, ...) {
    uint32_t a[6] = {0};
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc && i < 6; ++i) {
        a[i] = va_arg(ap, uint32_t);
    }
    va_end(ap);
#if defined(__arm__)
    typedef uint32_t(S3E_SOFTFP * guest6_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                             uint32_t);
    return ((guest6_fn)(uintptr_t)fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
#else
    return (uint32_t)arm_emu_call(S3E_GUEST_ARG(fn), argc, a);
#endif
}

uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t monotonic_ms(void) {
    return monotonic_us() / 1000u;
}

void sleep_ms(uint32_t ms) {
    plat_sleep_us((uint64_t)ms * 1000u);
}

void *open_first(const char *const *names) {
    for (size_t i = 0; names[i]; ++i) {
        void *handle = plat_lib_open(names[i]);
        if (handle) {
            return handle;
        }
    }
    return NULL;
}

void *lookup_gl(const char *symbol) {
    void *addr = egl_backend_get_gl_proc(symbol);
    if (g_gles2) {
        addr = addr ? addr : plat_lib_symbol(g_gles2, symbol);
    }
    if (!addr && g_gles1) {
        addr = plat_lib_symbol(g_gles1, symbol);
    }
    if (!addr && g_egl) {
        void *(*egl_get_proc_address)(const char *) = plat_lib_symbol(g_egl, "eglGetProcAddress");
        if (egl_get_proc_address) {
            addr = egl_get_proc_address(symbol);
        }
    }
    return addr;
}

void *lookup_egl(const char *symbol) {
    return g_egl ? plat_lib_symbol(g_egl, symbol) : NULL;
}

bool s3e_host_set_display_size(uint32_t width, uint32_t height) {
    if (!width || !height || width > UINT16_MAX || height > UINT16_MAX) {
        return false;
    }

    uint32_t surface_width = width;
    uint32_t surface_height = height;
    uint32_t minimum_height = (width * 3u + 2u) / 4u;
    uint32_t maximum_width = (height * 16u + 4u) / 9u;
    if (height > minimum_height) {
        surface_height = minimum_height;
    } else if (width > maximum_width + 1u) {
        surface_width = maximum_width;
    }

    if (surface_width > SIZE_MAX / surface_height ||
        surface_width * surface_height > SIZE_MAX / sizeof(*g_surface_pixels)) {
        return false;
    }

    uint32_t *pixels = calloc((size_t)surface_width * surface_height, sizeof(*pixels));
    if (!pixels) {
        return false;
    }

    free(g_surface_pixels);
    g_surface_pixels = pixels;
    g_native_window.width = (uint16_t)width;
    g_native_window.height = (uint16_t)height;
    g_surface.x = (uint16_t)((width - surface_width) / 2u);
    g_surface.y = (uint16_t)((height - surface_height) / 2u);
    g_surface.width = (uint16_t)surface_width;
    g_surface.height = (uint16_t)surface_height;
    g_pointer_x = (int32_t)((uint64_t)surface_width * 110u / 640u);
    g_pointer_y = (int32_t)((uint64_t)surface_height * 150u / 480u);
    return true;
}

bool s3e_host_init(const char *root) {
    g_host_start_us = monotonic_us();
    if (root && root[0]) {
        snprintf(g_root, sizeof(g_root), "%s", root);
    } else if (!getcwd(g_root, sizeof(g_root))) {
        snprintf(g_root, sizeof(g_root), ".");
    }
    if (!g_surface_pixels &&
        !s3e_host_set_display_size(g_native_window.width, g_native_window.height)) {
        fprintf(stderr, "failed to allocate %ux%u S3E surface\n", g_native_window.width,
                g_native_window.height);
        return false;
    }
    fprintf(stderr, "[display] drawable=%ux%u surface=%ux%u offset=%u,%u pitch=%zu\n",
            g_native_window.width, g_native_window.height, g_surface.width, g_surface.height,
            g_surface.x, g_surface.y, (size_t)g_surface.width * sizeof(*g_surface_pixels));
    return egl_backend_load_libraries();
}

static void clear_timers(void) {
    pthread_mutex_lock(&g_timer_mutex);
    struct timer_event *timer = g_timers;
    g_timers = NULL;
    pthread_mutex_unlock(&g_timer_mutex);
    while (timer) {
        struct timer_event *next = timer->next;
        free(timer);
        timer = next;
    }
}

static void close_all_memory_files(void) {
    while (g_memory_files) {
        struct memory_file *next = g_memory_files->next;
        fclose(g_memory_files->file);
        free(g_memory_files->buffer);
        free(g_memory_files);
        g_memory_files = next;
    }
}

void s3e_host_shutdown(void) {
    (void)s3eMemorySetUserMemMgr(NULL);
    s3e_zero_conf_shutdown();
    s3e_socket_shutdown();
    audio_shutdown();
    input_shutdown();
    egl_backend_shutdown();
    clear_timers();
    close_all_memory_files();
    free(g_surface_pixels);
    g_surface_pixels = NULL;
    s3e_memory_shutdown();
    for (size_t i = 0; i < g_stub_count; ++i) {
        free((void *)g_stub_names[i]);
    }
    g_stub_count = 0;
    if (g_stub_code) {
        plat_free(g_stub_code, g_stub_code_size);
        g_stub_code = NULL;
        g_stub_code_size = 0;
    }
    if (g_egl) {
        plat_lib_close(g_egl);
        g_egl = NULL;
    }
    if (g_gles1) {
        plat_lib_close(g_gles1);
        g_gles1 = NULL;
    }
    if (g_gles2) {
        plat_lib_close(g_gles2);
        g_gles2 = NULL;
    }
}
