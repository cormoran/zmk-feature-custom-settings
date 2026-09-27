/* SPDX-License-Identifier: MIT */
#pragma once
/* Included by custom_settings.h after the type enum and behavior payload.
 * BYTES/STRING/BEHAVIOR constructor literals live to the end of their enclosing
 * block (forever at file scope). They must not escape that lifetime. */
/* A value borrows BYTES/STRING/BEHAVIOR storage; INT32/BOOL stay inline.
 * A borrowed view is valid only inside its locked visitor. For a copied read,
 * initialize the output with ZMK_CUSTOM_SETTING_VIEW_BUFFER(buffer, capacity).
 * On entry to a copying read, size is buffer capacity; on success it becomes
 * payload length. Reinitialize before reuse. STRING payload length excludes NUL.
 * BEHAVIOR outputs require a buffer aligned for the behavior struct. */
struct zmk_custom_setting_value_view {
    uint8_t type;
    uint16_t size;
    union {
        const uint8_t *bytes_value;
        const char *string_value;
        int32_t int32_value;
        bool bool_value;
        const struct zmk_custom_setting_behavior_value *behavior_value;
    };
};

#define ZMK_CUSTOM_SETTING_VIEW_BUFFER(data_, capacity_)                                           \
    ((struct zmk_custom_setting_value_view){.bytes_value = (const uint8_t *)(data_),               \
                                            .size = MIN((size_t)(capacity_), UINT16_MAX)})

#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
#include <cormoran/zmk/custom_settings/compat/value.h>
#else
/* C has no typedef for struct tags. Preserve the old spelling as a tag alias,
 * without declaring a second layout or maintaining two equal C types. */
#define zmk_custom_setting_value zmk_custom_setting_value_view

#endif

#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
#define ZMK_CUSTOM_SETTING_VALUE_LOCAL(name_) struct zmk_custom_setting_value name_ = {0}
#else
#define ZMK_CUSTOM_SETTING_VALUE_LOCAL(name_)                                                      \
    _Alignas(struct zmk_custom_setting_behavior_value)                                             \
        uint8_t name_##_storage[MAX(CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1,                 \
                                    sizeof(struct zmk_custom_setting_behavior_value))];            \
    struct zmk_custom_setting_value name_ = {.bytes_value = name_##_storage,                       \
                                             .size = sizeof(name_##_storage)}
#endif

#define ZMK_CUSTOM_SETTING_VIEW_INT32(_value)                                                      \
    ((struct zmk_custom_setting_value_view){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,           \
                                            .int32_value = (_value)})

#define ZMK_CUSTOM_SETTING_VIEW_BOOL(_value)                                                       \
    ((struct zmk_custom_setting_value_view){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,            \
                                            .bool_value = (_value)})

#define ZMK_CUSTOM_SETTING_VIEW_STRING(_value)                                                     \
    ((struct zmk_custom_setting_value_view){                                                       \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,                                              \
        .size = (sizeof(_value) - 1) + ZERO_OR_COMPILE_ERROR(sizeof(_value) - 1 <= UINT16_MAX),    \
        .string_value = (_value)})

#define ZMK_CUSTOM_SETTING_VIEW_BYTES(...)                                                         \
    ((struct zmk_custom_setting_value_view){                                                       \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,                                               \
        .size = sizeof((uint8_t[]){__VA_ARGS__}) +                                                 \
                ZERO_OR_COMPILE_ERROR(sizeof((uint8_t[]){__VA_ARGS__}) <= UINT16_MAX),             \
        .bytes_value = (const uint8_t[]){__VA_ARGS__},                                             \
    })

#define ZMK_CUSTOM_SETTING_VIEW_BEHAVIOR(_behavior_id, _param1, _param2)                           \
    ((struct zmk_custom_setting_value_view){                                                       \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,                                            \
        .behavior_value =                                                                          \
            &(const struct zmk_custom_setting_behavior_value){                                     \
                .behavior_id = (_behavior_id), .param1 = (_param1), .param2 = (_param2)},          \
    })

#ifndef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
#define ZMK_CUSTOM_SETTING_VALUE_INT32 ZMK_CUSTOM_SETTING_VIEW_INT32
#define ZMK_CUSTOM_SETTING_VALUE_BOOL ZMK_CUSTOM_SETTING_VIEW_BOOL
#define ZMK_CUSTOM_SETTING_VALUE_STRING ZMK_CUSTOM_SETTING_VIEW_STRING
#define ZMK_CUSTOM_SETTING_VALUE_BYTES ZMK_CUSTOM_SETTING_VIEW_BYTES
#define ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR ZMK_CUSTOM_SETTING_VIEW_BEHAVIOR
#endif
