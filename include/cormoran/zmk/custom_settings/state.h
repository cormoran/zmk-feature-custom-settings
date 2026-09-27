/* SPDX-License-Identifier: MIT */
#pragma once
/* A descriptor already points to its state. Store a typed payload immediately
 * after the flag header, rather than add another pointer and a second object.
 * Array parents need only flags: their element payloads live in dense arrays. */
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
#include <cormoran/zmk/custom_settings/compat/state.h>
#define ZMK_CUSTOM_SETTING_STATE_DEFINE(name_, type_)                                              \
    static struct zmk_custom_setting_state name_ = {0}
#define ZMK_CUSTOM_SETTING_STATE_HEADER(name_) (&(name_))
#else
struct zmk_custom_setting_state {
    uint8_t flags;
};
struct zmk_custom_setting_int32_state {
    struct zmk_custom_setting_state header;
    int32_t value;
};
struct zmk_custom_setting_bool_state {
    struct zmk_custom_setting_state header;
    bool value;
};
struct zmk_custom_setting_behavior_state {
    struct zmk_custom_setting_state header;
    struct zmk_custom_setting_behavior_value value;
};
struct zmk_custom_setting_blob_state {
    struct zmk_custom_setting_state header;
    struct zmk_custom_setting_blob value;
};
/* Compile-time type selection: no union, heap allocation or runtime branch. */
#define ZMK_CUSTOM_SETTING_STATE_DEFINE(name_, type_)                                              \
    static __typeof__(__builtin_choose_expr(                                                       \
        (type_) == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                            \
        (struct zmk_custom_setting_int32_state){0},                                                \
        __builtin_choose_expr(                                                                     \
            (type_) == ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,                                         \
            (struct zmk_custom_setting_bool_state){0},                                             \
            __builtin_choose_expr((type_) == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,               \
                                  (struct zmk_custom_setting_behavior_state){0},                   \
                                  (struct zmk_custom_setting_blob_state){0})))) name_ = {0}
#define ZMK_CUSTOM_SETTING_STATE_HEADER(name_) (&(name_).header)
#endif

/* Use only the accessor matching a non-array setting's declared type.
 * Keyspace slots always store a blob, regardless of the presented payload type.
 * Callers hold the settings lock while inspecting or mutating storage. */
static inline int32_t *zmk_custom_setting_state_int32(struct zmk_custom_setting_state *state) {
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
    return &state->int32_value;
#else
    return &CONTAINER_OF(state, struct zmk_custom_setting_int32_state, header)->value;
#endif
}
static inline bool *zmk_custom_setting_state_bool(struct zmk_custom_setting_state *state) {
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
    return &state->bool_value;
#else
    return &CONTAINER_OF(state, struct zmk_custom_setting_bool_state, header)->value;
#endif
}
static inline struct zmk_custom_setting_behavior_value *
zmk_custom_setting_state_behavior(struct zmk_custom_setting_state *state) {
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
    return &state->behavior;
#else
    return &CONTAINER_OF(state, struct zmk_custom_setting_behavior_state, header)->value;
#endif
}
static inline struct zmk_custom_setting_blob *
zmk_custom_setting_state_blob(struct zmk_custom_setting_state *state) {
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
    return &state->blob;
#else
    return &CONTAINER_OF(state, struct zmk_custom_setting_blob_state, header)->value;
#endif
}
