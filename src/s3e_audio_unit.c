#include "s3e_host_internal.h"

enum {
    IS_AUDIO_UNIT_INTERFACE_ENTRIES = 13,
};

static int32_t is_audio_unit_register(uint32_t id, void *callback, void *user_data) {
    (void)id;
    (void)callback;
    (void)user_data;
    return 0;
}

static int32_t is_audio_unit_unregister(uint32_t id, void *callback) {
    (void)id;
    (void)callback;
    return 0;
}

static int32_t is_audio_unit_init(void) {
    return 0;
}

static int32_t is_audio_unit_terminate(void) {
    audio_unit_backend_shutdown();
    return 0;
}

static int32_t is_audio_unit_set_callbacks(void *capture_callback, void *render_callback) {
    return audio_unit_backend_set_callbacks(capture_callback, render_callback);
}

static int32_t is_audio_unit_start(void) {
    return audio_unit_backend_start();
}

static int32_t is_audio_unit_stop(void) {
    return audio_unit_backend_stop();
}

int32_t is_audio_unit_get_interface(void *iface, uint32_t size) {
    uint32_t table[IS_AUDIO_UNIT_INTERFACE_ENTRIES] = {
        GUEST_ADDRESS(&is_audio_unit_register),
        GUEST_ADDRESS(&is_audio_unit_unregister),
        GUEST_ADDRESS(&is_audio_unit_init),
        GUEST_ADDRESS(&is_audio_unit_terminate),
        GUEST_ADDRESS(&is_audio_unit_set_callbacks),
        GUEST_ADDRESS(&is_audio_unit_start),
        GUEST_ADDRESS(&is_audio_unit_stop),
        GUEST_ADDRESS(&s3eAudioPlay),
        GUEST_ADDRESS(&s3eAudioStop),
        GUEST_ADDRESS(&s3eAudioResume),
        GUEST_ADDRESS(&s3eAudioPause),
        GUEST_ADDRESS(&s3eAudioSetInt),
        GUEST_ADDRESS(&s3eAudioGetInt),
    };

    if (!iface || size != sizeof(table)) {
        return 1;
    }
    memcpy(iface, table, sizeof(table));
    return 0;
}
