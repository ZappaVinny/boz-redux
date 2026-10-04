#include "s3e_host_internal.h"
#include "mod_runtime.h"
#include "native_input.h"

#include <stdarg.h>

uint32_t s3e_guest_call(const void *fn, int argc, ...) {
    uint32_t a[6] = {0};
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc && i < 6; ++i) {
        a[i] = va_arg(ap, uint32_t);
    }
    va_end(ap);
    typedef uint32_t (*native6_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    return ((native6_fn)(uintptr_t)fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
}

__attribute__((weak)) void present_rect(int32_t width, int32_t height, int32_t *rect) {
    rect[0] = 0;
    rect[1] = 0;
    rect[2] = width;
    rect[3] = height;
}

__attribute__((weak)) int32_t s3eDeviceRequestQuit(void) {
    return 0;
}

/* Mod runtime hooks used by s3e_input.c; tests run without mods. */
__attribute__((weak)) void overlay_event(const struct overlay_event *event) {
    (void)event;
}

__attribute__((weak)) bool overlay_capturing(void) {
    return false;
}

__attribute__((weak)) bool lua_runtime_key(const char *name, bool down, bool repeat) {
    (void)name;
    (void)down;
    (void)repeat;
    return false;
}

__attribute__((weak)) int egl_backend_drawable_size(int *width, int *height) {
    (void)width;
    (void)height;
    return 0;
}

/* Native controls need the game; tests use the touchpad path. */
__attribute__((weak)) bool native_input_available(void) {
    return false;
}

__attribute__((weak)) enum native_match native_input_match(void) {
    return NATIVE_MATCH_NONE;
}

__attribute__((weak)) void native_input_look(float dx, float dy) {
    (void)dx;
    (void)dy;
}

__attribute__((weak)) void native_input_move(float x, float y, bool sprint) {
    (void)x;
    (void)y;
    (void)sprint;
}

__attribute__((weak)) void native_input_action(enum native_action action, bool down) {
    (void)action;
    (void)down;
}

__attribute__((weak)) void native_input_release(void) {}

__attribute__((weak)) bool lua_runtime_action(const char *name, bool down) {
    (void)name;
    (void)down;
    return false;
}

__attribute__((weak)) int native_input_action_from_name(const char *name) {
    (void)name;
    return -1;
}

__attribute__((weak)) void native_input_send(enum native_action action, bool down) {
    (void)action;
    (void)down;
}

__attribute__((weak)) bool native_input_action_down(enum native_action action) {
    (void)action;
    return false;
}

__attribute__((weak)) bool native_input_aiming(void) {
    return false;
}
