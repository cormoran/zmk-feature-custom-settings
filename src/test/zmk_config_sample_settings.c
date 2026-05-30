/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <cormoran/zmk/custom_settings.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>

static struct zmk_rpc_custom_subsystem_meta zmk_config_sample_custom_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool zmk_config_sample_handle_request(const zmk_custom_CallRequest *req,
                                             pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);

    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(zmk_config_sample, &zmk_config_sample_custom_settings_meta,
                         zmk_config_sample_handle_request);
#endif

static int zmk_config_sample_reverse_bytes(const struct zmk_custom_setting *setting,
                                           const uint8_t *src, size_t src_size, uint8_t *dest,
                                           size_t *dest_size, size_t dest_capacity) {
    ARG_UNUSED(setting);

    if (src_size > dest_capacity) {
        return -EMSGSIZE;
    }

    for (size_t i = 0; i < src_size; i++) {
        dest[i] = src[src_size - i - 1];
    }
    *dest_size = src_size;
    return 0;
}

static const struct zmk_custom_setting_constraint zmk_config_sample_no_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_NONE},
};

static const struct zmk_custom_setting_constraint zmk_config_sample_range_0_100[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 100}}},
};

static const struct zmk_custom_setting_constraint zmk_config_sample_range_0_10[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
     .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
               .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 10}}},
};

static const struct zmk_custom_setting_constraint zmk_config_sample_hid_usage_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE,
     .hid_usage = {.usage_page = HID_USAGE_KEY,
                   .usage_min = HID_USAGE_KEY_KEYBOARD_A,
                   .usage_max = HID_USAGE_KEY_KEYBOARD_Z}},
};

static const struct zmk_custom_setting_constraint zmk_config_sample_layer_id_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID},
};

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
static const struct zmk_custom_setting_constraint zmk_config_sample_behavior_id_constraints[] = {
    {.type = ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID},
};
#endif

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_int32) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "int32_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_range_0_100,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_range_0_100),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 42},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_bool) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "bool_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, .bool_value = true},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_string) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "string_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                      .size = sizeof("hello zmk") - 1,
                      .string_value = "hello zmk"},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_bytes) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "bytes_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                      .size = 4,
                      .bytes_value = {0x01, 0x02, 0x03, 0x04}},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_bytes_rpc) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "bytes_rpc_value",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                      .size = 4,
                      .bytes_value = {0x11, 0x22, 0x33, 0x44}},
    .rpc_serializer = zmk_config_sample_reverse_bytes,
    .rpc_deserializer = zmk_config_sample_reverse_bytes,
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_private_string) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "device_private_string",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_no_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_no_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                      .size = sizeof("device only") - 1,
                      .string_value = "device only"},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_secure_int32) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "secure_int32",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    .constraints = zmk_config_sample_range_0_10,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_range_0_10),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 7},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_hid_usage) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "hid_usage",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_hid_usage_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_hid_usage_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                      .int32_value = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_A)},
};

STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_layer_id) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "layer_id",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_layer_id_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_layer_id_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
};

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
STRUCT_SECTION_ITERABLE(zmk_custom_setting, zmk_config_sample_behavior_id) = {
    .custom_subsystem_id = "zmk_config_sample",
    .key = "behavior_id",
    .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
    .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    .constraints = zmk_config_sample_behavior_id_constraints,
    .constraints_count = ARRAY_SIZE(zmk_config_sample_behavior_id_constraints),
    .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
};
#endif

#define ZMK_CONFIG_SAMPLE_ARRAY_SETTING(_name, _index, _default)                                   \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .custom_subsystem_id = "zmk_config_sample",                                                \
        .key = "array_value/" ZMK_CUSTOM_SETTINGS_STRINGIFY(_index),                               \
        .array_key = "array_value",                                                                \
        .array_index = (_index),                                                                   \
        .array_max_size = 4,                                                                       \
        .default_array_size = 4,                                                                   \
        .array_size = 4,                                                                           \
        .persistent_array_size = 4,                                                                \
        .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                         \
        .confidentiality = ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                          \
        .read_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                 \
        .write_permission = ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                                \
        .constraints = zmk_config_sample_range_0_100,                                              \
        .constraints_count = ARRAY_SIZE(zmk_config_sample_range_0_100),                            \
        .default_value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_default)}, \
    }

ZMK_CONFIG_SAMPLE_ARRAY_SETTING(zmk_config_sample_array_0, 0, 10);
ZMK_CONFIG_SAMPLE_ARRAY_SETTING(zmk_config_sample_array_1, 1, 20);
ZMK_CONFIG_SAMPLE_ARRAY_SETTING(zmk_config_sample_array_2, 2, 30);
ZMK_CONFIG_SAMPLE_ARRAY_SETTING(zmk_config_sample_array_3, 3, 40);
