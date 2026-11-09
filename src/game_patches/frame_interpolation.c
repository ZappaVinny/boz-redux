#include "game_patches.h"
#include "mod_runtime.h"
#include "s3e_host_internal.h"
#include "arm_emu.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The director (the game's main loop) calls two callbacks from slots: one for each fixed step and
 * one to interpolate between steps, which the shipped game leaves empty. The fixed one is wrapped
 * to capture the transforms, the empty one replaced to blend them (TransformSet_ApplyFactor). */
enum {
    INTERPOLATION_CHANNEL = 1,
    FIXED_FACTOR_ONE = 4096,
};

/* Guest addresses of the game's functions and data. */
static uint32_t g_active_set_pointer;
static uint32_t g_original_fixed_callback;
static uint32_t g_capture_transforms;
static uint32_t g_apply_factor;

/* The callbacks' signature for guest-base builds: the director is a pointer. */
#define DIRECTOR_CALLBACK_SIGNATURE "i:piiiii"

static bool code_matches(uint32_t address, const uint8_t *expected, size_t size) {
    return address && memcmp(GUEST_POINTER(address & ~1u), expected, size) == 0;
}

static uint32_t read32(const void *address) {
    uint32_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

static void write32(void *address, uint32_t value) {
    memcpy(address, &value, sizeof(value));
}

static void *active_transform_set(void) {
    return GUEST_POINTER(read32(GUEST_POINTER(g_active_set_pointer)));
}

static int32_t interpolation_factor(uint32_t bits) {
    float factor;
    memcpy(&factor, &bits, sizeof(factor));
    if (!isfinite(factor) || factor <= 0.0f) {
        return 0;
    }
    if (factor >= 1.0f) {
        return FIXED_FACTOR_ONE;
    }
    return (int32_t)(factor * (float)FIXED_FACTOR_ONE);
}

static S3E_SOFTFP uint32_t director_fixed_callback(void *director, uint32_t channel, uint32_t step,
                                                   uint32_t value, uint32_t argument4,
                                                   uint32_t argument5) {
    if (channel == INTERPOLATION_CHANNEL) {
        void *active_set = active_transform_set();
        if (active_set) {
            s3e_guest_call(g_capture_transforms, 1, S3E_GUEST_ARG(active_set));
        }
    }
    return s3e_guest_call(g_original_fixed_callback, 6, S3E_GUEST_ARG(director), channel, step,
                          value, argument4, argument5);
}

static S3E_SOFTFP uint32_t director_interpolate_callback(void *director, uint32_t channel,
                                                         uint32_t step, uint32_t factor_bits,
                                                         uint32_t argument4, uint32_t argument5) {
    (void)step;
    (void)argument4;
    (void)argument5;
    if (channel == INTERPOLATION_CHANNEL) {
        void *active_set = active_transform_set();
        if (active_set) {
            s3e_guest_call(g_apply_factor, 2, S3E_GUEST_ARG(active_set),
                           (uint32_t)interpolation_factor(factor_bits));
        }
    }
    return GUEST_ADDRESS(director);
}

bool frame_interpolation_install(void) {
    static const uint8_t fixed_signature[] = {
        0x02, 0x29, 0x2d, 0xe9, 0xf0, 0x41, 0x80, 0x46,
        0x0f, 0x46, 0x16, 0x46, 0xdd, 0xe9, 0x06, 0x45,
    };
    static const uint8_t interpolate_signature[] = {0x70, 0x47};
    static const uint8_t capture_signature[] = {
        0x38, 0xb5, 0x05, 0x46, 0x00, 0x24, 0xab, 0x68,
        0x9c, 0x42, 0x06, 0xd2, 0x6b, 0x68, 0x53, 0xf8,
    };
    static const uint8_t apply_signature[] = {
        0x70, 0xb5, 0x05, 0x46, 0x0e, 0x46, 0x00, 0x24,
        0xab, 0x68, 0x9c, 0x42, 0x07, 0xd2, 0x6b, 0x68,
    };

    uint32_t active_set = game_symbol_address("g_activeTransformSet");
    uint32_t fixed_slot = game_symbol_address("g_directorFixedCallback");
    uint32_t interpolate_slot = game_symbol_address("g_directorInterpolateCallback");
    uint32_t fixed = game_symbol_address("Director_FixedStepCallback");
    uint32_t interpolate = game_symbol_address("Director_InterpolateCallback");
    uint32_t capture = game_symbol_address("TransformSet_Capture");
    uint32_t apply = game_symbol_address("TransformSet_ApplyFactor");
    if (!active_set || !fixed_slot || !interpolate_slot ||
        !code_matches(fixed, fixed_signature, sizeof(fixed_signature)) ||
        !code_matches(interpolate, interpolate_signature, sizeof(interpolate_signature)) ||
        !code_matches(capture, capture_signature, sizeof(capture_signature)) ||
        !code_matches(apply, apply_signature, sizeof(apply_signature)) ||
        read32(GUEST_POINTER(fixed_slot)) != fixed ||
        read32(GUEST_POINTER(interpolate_slot)) != interpolate) {
        return false;
    }

    g_active_set_pointer = active_set;
    g_original_fixed_callback = fixed;
    g_capture_transforms = capture;
    g_apply_factor = apply;
    write32(GUEST_POINTER(fixed_slot),
            arm_emu_guest_fn_sig((const void *)(uintptr_t)director_fixed_callback,
                                 DIRECTOR_CALLBACK_SIGNATURE));
    write32(GUEST_POINTER(interpolate_slot),
            arm_emu_guest_fn_sig((const void *)(uintptr_t)director_interpolate_callback,
                                 DIRECTOR_CALLBACK_SIGNATURE));
    return true;
}
