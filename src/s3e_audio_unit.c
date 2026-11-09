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
        S3E_GUEST_FN(&is_audio_unit_register),
        S3E_GUEST_FN(&is_audio_unit_unregister),
        S3E_GUEST_FN(&is_audio_unit_init),
        S3E_GUEST_FN(&is_audio_unit_terminate),
        S3E_GUEST_FN(&is_audio_unit_set_callbacks),
        S3E_GUEST_FN(&is_audio_unit_start),
        S3E_GUEST_FN(&is_audio_unit_stop),
        S3E_GUEST_FN(&s3eAudioPlay),
        S3E_GUEST_FN(&s3eAudioStop),
        S3E_GUEST_FN(&s3eAudioResume),
        S3E_GUEST_FN(&s3eAudioPause),
        S3E_GUEST_FN(&s3eAudioSetInt),
        S3E_GUEST_FN(&s3eAudioGetInt),
    };

    if (!iface || size != sizeof(table)) {
        return 1;
    }
    memcpy(iface, table, sizeof(table));
    return 0;
}
