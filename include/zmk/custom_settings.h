/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>

#define ZMK_CUSTOM_SETTING_SOURCE_LOCAL 0U
#define ZMK_CUSTOM_SETTING_SOURCE_ALL UINT32_MAX

enum zmk_custom_setting_value_type {
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES = 1,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 = 2,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL = 3,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING = 4,
};

enum zmk_custom_setting_write_mode {
    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,
    ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST,
    ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY,
};

enum zmk_custom_setting_confidentiality {
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE,
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
};

enum zmk_custom_setting_permission {
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
};

enum zmk_custom_setting_constraint_type {
    ZMK_CUSTOM_SETTING_CONSTRAINT_NONE,
    ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
    ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS,
    ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE,
    ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID,
    ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID,
};

enum zmk_custom_setting_changed_kind {
    ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED,
    ZMK_CUSTOM_SETTING_CHANGED_SAVED,
    ZMK_CUSTOM_SETTING_CHANGED_DISCARDED,
    ZMK_CUSTOM_SETTING_CHANGED_RESET,
};

struct zmk_custom_setting_value {
    enum zmk_custom_setting_value_type type;
    size_t size;
    uint8_t bytes_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    int32_t int32_value;
    bool bool_value;
    char string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
};

struct zmk_custom_setting_range {
    struct zmk_custom_setting_value min;
    struct zmk_custom_setting_value max;
};

struct zmk_custom_setting_options {
    const struct zmk_custom_setting_value *values;
    const char *const *labels;
    size_t count;
};

struct zmk_custom_setting_hid_usage {
    uint32_t usage_page;
    uint32_t usage_min;
    uint32_t usage_max;
};

struct zmk_custom_setting_constraint {
    enum zmk_custom_setting_constraint_type type;
    union {
        struct zmk_custom_setting_range range;
        struct zmk_custom_setting_options options;
        struct zmk_custom_setting_hid_usage hid_usage;
    };
};

struct zmk_custom_setting {
    const char *subsystem_id;
    const char *key;
    enum zmk_custom_setting_value_type value_type;
    enum zmk_custom_setting_confidentiality confidentiality;
    enum zmk_custom_setting_permission read_permission;
    enum zmk_custom_setting_permission write_permission;
    struct zmk_custom_setting_constraint constraint;
    struct zmk_custom_setting_value default_value;

    bool initialized;
    bool has_persistent_value;
    bool temporary_active;
    struct zmk_custom_setting_value persistent_value;
    struct zmk_custom_setting_value memory_value;
    struct zmk_custom_setting_value temporary_value;
};

struct zmk_custom_setting_changed {
    const struct zmk_custom_setting *setting;
    enum zmk_custom_setting_changed_kind kind;
    uint32_t source;
};

ZMK_EVENT_DECLARE(zmk_custom_setting_changed);

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

#define ZMK_CUSTOM_SETTING_NO_CONSTRAINT                                                           \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_NONE})

#define ZMK_CUSTOM_SETTING_RANGE_INT32(_min, _max)                                                 \
    ((struct zmk_custom_setting_constraint){                                                       \
        .type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,                                               \
        .range = {.min = ZMK_CUSTOM_SETTING_VALUE_INT32(_min),                                     \
                  .max = ZMK_CUSTOM_SETTING_VALUE_INT32(_max)}})

#define ZMK_CUSTOM_SETTING_HID_USAGE(_usage_page, _usage_min, _usage_max)                          \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE,       \
                                            .hid_usage = {.usage_page = (_usage_page),             \
                                                          .usage_min = (_usage_min),               \
                                                          .usage_max = (_usage_max)}})

#define ZMK_CUSTOM_SETTING_LAYER_ID                                                                \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID})

#define ZMK_CUSTOM_SETTING_BEHAVIOR_ID                                                             \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID})

#define ZMK_CUSTOM_SETTING_DEFINE(_name, _subsystem_id, _key, _value_type, _default_value,         \
                                  _confidentiality, _read_permission, _write_permission,           \
                                  _constraint)                                                     \
    BUILD_ASSERT(sizeof(_subsystem_id) <= CONFIG_ZMK_CUSTOM_SETTINGS_SUBSYSTEM_ID_MAX_LEN,         \
                 "Custom setting subsystem id is too long");                                       \
    BUILD_ASSERT(sizeof(_key) <= CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                           \
                 "Custom setting key is too long");                                                \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .subsystem_id = _subsystem_id,                                                             \
        .key = _key,                                                                               \
        .value_type = _value_type,                                                                 \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraint = _constraint,                                                                 \
        .default_value = _default_value,                                                           \
    }

#define ZMK_CUSTOM_SETTING_FOREACH(_var) STRUCT_SECTION_FOREACH(zmk_custom_setting, _var)

const struct zmk_custom_setting *zmk_custom_setting_find(const char *subsystem_id, const char *key);
bool zmk_custom_setting_matches(const struct zmk_custom_setting *setting, const char *subsystem_id,
                                const char *key, const char *key_prefix);

int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                            struct zmk_custom_setting_value *value);
int zmk_custom_setting_read_by_key(const char *subsystem_id, const char *key,
                                   struct zmk_custom_setting_value *value);

int zmk_custom_setting_write(const struct zmk_custom_setting *setting,
                             const struct zmk_custom_setting_value *value,
                             enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_write_by_key(const char *subsystem_id, const char *key,
                                    const struct zmk_custom_setting_value *value,
                                    enum zmk_custom_setting_write_mode mode);

int zmk_custom_setting_save(const struct zmk_custom_setting *setting);
int zmk_custom_setting_discard(const struct zmk_custom_setting *setting);
int zmk_custom_setting_reset(const struct zmk_custom_setting *setting);
int zmk_custom_setting_rollback_temporary(const struct zmk_custom_setting *setting);

int zmk_custom_settings_save_scope(const char *subsystem_id, const char *key,
                                   const char *key_prefix, uint32_t *affected_count);
int zmk_custom_settings_discard_scope(const char *subsystem_id, const char *key,
                                      const char *key_prefix, uint32_t *affected_count);
int zmk_custom_settings_reset_scope(const char *subsystem_id, const char *key,
                                    const char *key_prefix, uint32_t *affected_count);

bool zmk_custom_setting_has_unsaved_value(const struct zmk_custom_setting *setting);
int zmk_custom_setting_validate(const struct zmk_custom_setting *setting,
                                const struct zmk_custom_setting_value *value);
