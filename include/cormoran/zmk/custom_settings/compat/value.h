/* SPDX-License-Identifier: MIT */
#pragma once
/* The owning carrier exists only at the legacy API boundary. */
struct zmk_custom_setting_value {
    enum zmk_custom_setting_value_type type;
    size_t size;
    union {
        uint8_t bytes_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
        int32_t int32_value;
        bool bool_value;
        char string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
        struct zmk_custom_setting_behavior_value behavior_value;
    };
};

#define ZMK_CUSTOM_SETTING_VALUE_INT32(_value)                                                     \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                \
                                       .int32_value = (_value)})

#define ZMK_CUSTOM_SETTING_VALUE_BOOL(_value)                                                      \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,                 \
                                       .bool_value = (_value)})

#define ZMK_CUSTOM_SETTING_VALUE_STRING(_value)                                                    \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,               \
                                       .size = sizeof(_value) - 1,                                 \
                                       .string_value = (_value)})

#define ZMK_CUSTOM_SETTING_VALUE_BYTES(...)                                                        \
    ((struct zmk_custom_setting_value){                                                            \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,                                               \
        .size = sizeof((uint8_t[]){__VA_ARGS__}),                                                  \
        .bytes_value = {__VA_ARGS__},                                                              \
    })

#define ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(_behavior_id, _param1, _param2)                          \
    ((struct zmk_custom_setting_value){                                                            \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,                                            \
        .behavior_value = {.behavior_id = (_behavior_id),                                          \
                           .param1 = (_param1),                                                    \
                           .param2 = (_param2)},                                                   \
    })
