/*
 * Copyright (c) 2026 cormoran
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
#define ZMK_CUSTOM_SETTING_ARRAY_NONE UINT32_MAX

/* Supported scalar value types for custom settings. */
enum zmk_custom_setting_value_type {
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES = 1,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 = 2,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL = 3,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING = 4,
};

/* Write mode controls whether a new value is saved, staged in RAM, or temporary. */
enum zmk_custom_setting_write_mode {
    ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,
    ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST,
    ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY,
};

/* Controls whether RPC may expose the setting value. */
enum zmk_custom_setting_confidentiality {
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE,
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
};

/* Secure settings require Studio unlock before RPC read/write access. */
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
    union {
        uint8_t bytes_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
        int32_t int32_value;
        bool bool_value;
        char string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
    };
};

struct zmk_custom_setting;

typedef int (*zmk_custom_setting_rpc_bytes_converter_t)(const struct zmk_custom_setting *setting,
                                                        const uint8_t *src, size_t src_size,
                                                        uint8_t *dest, size_t *dest_size,
                                                        size_t dest_capacity);

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
    const char *custom_subsystem_id;
    const char *key;
    const char *array_key;
    uint32_t array_index;
    uint32_t array_max_size;
    uint32_t default_array_size;
    uint32_t array_size;
    uint32_t persistent_array_size;
    enum zmk_custom_setting_value_type value_type;
    enum zmk_custom_setting_confidentiality confidentiality;
    enum zmk_custom_setting_permission read_permission;
    enum zmk_custom_setting_permission write_permission;
    const struct zmk_custom_setting_constraint *constraints;
    size_t constraints_count;
    struct zmk_custom_setting_value default_value;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;

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

#define ZMK_CUSTOM_SETTINGS_STRINGIFY(x) ZMK_CUSTOM_SETTINGS_STRINGIFY_INNER(x)
#define ZMK_CUSTOM_SETTINGS_STRINGIFY_INNER(x) #x

#define ZMK_CUSTOM_SETTING_NO_CONSTRAINT                                                           \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_NONE})

/* Field initializers (not compound literals) so this expands to a constant
 * expression usable in a static initializer even under strict -std=c11; GCC
 * rejects an outer compound literal that nests another compound literal. */
#define ZMK_CUSTOM_SETTING_RANGE_INT32(_min, _max)                                                 \
    ((struct zmk_custom_setting_constraint){                                                       \
        .type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,                                               \
        .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_min)},     \
                  .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_max)}}})

#define ZMK_CUSTOM_SETTING_HID_USAGE(_usage_page, _usage_min, _usage_max)                          \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE,       \
                                            .hid_usage = {.usage_page = (_usage_page),             \
                                                          .usage_min = (_usage_min),               \
                                                          .usage_max = (_usage_max)}})

#define ZMK_CUSTOM_SETTING_LAYER_ID                                                                \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID})

#define ZMK_CUSTOM_SETTING_BEHAVIOR_ID                                                             \
    ((struct zmk_custom_setting_constraint){.type = ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID})

/* Register one custom setting in the iterable setting registry. */
#define ZMK_CUSTOM_SETTING_DEFINE(_name, _custom_subsystem_id, _key, _value_type, _default_value,  \
                                  _confidentiality, _read_permission, _write_permission,           \
                                  _constraint)                                                     \
    ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(_name, _custom_subsystem_id, _key, _value_type,     \
                                               _default_value, _confidentiality, _read_permission, \
                                               _write_permission, _constraint)

/* Register one custom setting with custom bytes RPC conversion hooks. */
#define ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS(                                             \
    _name, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,              \
    _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, _constraint)          \
    ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                                 \
        _name, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,          \
        _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, _constraint)

/* Register one custom setting with zero or more constraints. */
#define ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(_name, _custom_subsystem_id, _key, _value_type, \
                                                   _default_value, _confidentiality,               \
                                                   _read_permission, _write_permission, ...)       \
    ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                                 \
        _name, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,          \
        _read_permission, _write_permission, NULL, NULL, __VA_ARGS__)

#define ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                             \
    _name, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,              \
    _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, ...)                  \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT(sizeof(_key) <= CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                           \
                 "Custom setting key is too long");                                                \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key = _key,                                                                               \
        .array_key = NULL,                                                                         \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .array_max_size = 0,                                                                       \
        .default_array_size = 0,                                                                   \
        .array_size = 0,                                                                           \
        .persistent_array_size = 0,                                                                \
        .value_type = _value_type,                                                                 \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = _default_value,                                                           \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
    }

/* Register one element of an array setting. _array_size is the maximum length. */
#define ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(                                                   \
    _name, _custom_subsystem_id, _key, _index, _array_size, _value_type, _default_value,           \
    _confidentiality, _read_permission, _write_permission, _constraint)                            \
    ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE_WITH_CONSTRAINTS(                                      \
        _name, _custom_subsystem_id, _key, _index, _array_size, _value_type, _default_value,       \
        _confidentiality, _read_permission, _write_permission, _constraint)

/* Register one array element with custom bytes RPC conversion hooks. */
#define ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE_WITH_RPC_CONVERTERS(                               \
    _name, _custom_subsystem_id, _key, _index, _array_size, _value_type, _default_value,           \
    _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer,     \
    _constraint)                                                                                   \
    ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                   \
        _name, _custom_subsystem_id, _key, _index, _array_size, _value_type, _default_value,       \
        _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, \
        _constraint)

#define ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE_WITH_CONSTRAINTS(                                  \
    _name, _custom_subsystem_id, _key, _index, _array_size, _value_type, _default_value,           \
    _confidentiality, _read_permission, _write_permission, ...)                                    \
    ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                   \
        _name, _custom_subsystem_id, _key, _index, _array_size, _value_type, _default_value,       \
        _confidentiality, _read_permission, _write_permission, NULL, NULL, __VA_ARGS__)

#define ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(               \
    _name, _custom_subsystem_id, _key, _index, _array_size, _value_type, _default_value,           \
    _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer,     \
    ...)                                                                                           \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT(sizeof(_key "/" ZMK_CUSTOM_SETTINGS_STRINGIFY(_index)) <=                         \
                     CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                                       \
                 "Custom setting array element key is too long");                                  \
    BUILD_ASSERT((_index) < (_array_size), "Custom setting array index must be in range");         \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key = _key "/" ZMK_CUSTOM_SETTINGS_STRINGIFY(_index),                                     \
        .array_key = _key,                                                                         \
        .array_index = (_index),                                                                   \
        .array_max_size = (_array_size),                                                           \
        .default_array_size = (_array_size),                                                       \
        .array_size = (_array_size),                                                               \
        .persistent_array_size = (_array_size),                                                    \
        .value_type = _value_type,                                                                 \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = _default_value,                                                           \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
    }

#define ZMK_CUSTOM_SETTING_FOREACH(_var) STRUCT_SECTION_FOREACH(zmk_custom_setting, _var)

const struct zmk_custom_setting *zmk_custom_setting_find(const char *custom_subsystem_id,
                                                         const char *key);
/* Find any element of an array setting by its public base key. */
const struct zmk_custom_setting *zmk_custom_setting_find_array(const char *custom_subsystem_id,
                                                               const char *key);
/* Find one element of an array setting by its public base key and element index. */
const struct zmk_custom_setting *
zmk_custom_setting_find_array_element(const char *custom_subsystem_id, const char *key,
                                      uint32_t index);
/* Return the public key exposed by RPC. Array elements return their base key. */
const char *zmk_custom_setting_public_key(const struct zmk_custom_setting *setting);
bool zmk_custom_setting_is_array(const struct zmk_custom_setting *setting);
/* Return the active array length for an array element. */
uint32_t zmk_custom_setting_array_size(const struct zmk_custom_setting *setting);
/* Return the maximum array length allocated by the registry. */
uint32_t zmk_custom_setting_array_max_size(const struct zmk_custom_setting *setting);
/* Match a setting against optional subsystem, exact key, and key-prefix filters. */
bool zmk_custom_setting_matches(const struct zmk_custom_setting *setting,
                                const char *custom_subsystem_id, const char *key,
                                const char *key_prefix);

/* Read the effective value. A temporary override takes precedence over memory value. */
int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                            struct zmk_custom_setting_value *value);
int zmk_custom_setting_read_by_key(const char *custom_subsystem_id, const char *key,
                                   struct zmk_custom_setting_value *value);
/* Read one array setting element by its public base key and element index. */
int zmk_custom_setting_read_array_by_key(const char *custom_subsystem_id, const char *key,
                                         uint32_t index, struct zmk_custom_setting_value *value);
/* Convert an internal bytes value into its RPC bytes representation. */
int zmk_custom_setting_serialize_rpc_value(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *internal_value,
                                           struct zmk_custom_setting_value *rpc_value);
/* Convert an RPC bytes value into its internal firmware representation. */
int zmk_custom_setting_deserialize_rpc_value(const struct zmk_custom_setting *setting,
                                             const struct zmk_custom_setting_value *rpc_value,
                                             struct zmk_custom_setting_value *internal_value);

/* Write a value using the selected memory, persist, or temporary mode. */
int zmk_custom_setting_write(const struct zmk_custom_setting *setting,
                             const struct zmk_custom_setting_value *value,
                             enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_write_by_key(const char *custom_subsystem_id, const char *key,
                                    const struct zmk_custom_setting_value *value,
                                    enum zmk_custom_setting_write_mode mode);
/* Write one array setting element by its public base key and element index. */
int zmk_custom_setting_write_array_by_key(const char *custom_subsystem_id, const char *key,
                                          uint32_t index,
                                          const struct zmk_custom_setting_value *value,
                                          enum zmk_custom_setting_write_mode mode);
/* Write one array setting element and update the active array length. */
int zmk_custom_setting_write_array_element(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *value,
                                           uint32_t array_size,
                                           enum zmk_custom_setting_write_mode mode);
/* Append one value to an array setting and grow the active length by one. */
int zmk_custom_setting_array_push_back(const struct zmk_custom_setting *setting,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode);
/* Remove the last active array element and optionally return its previous value. */
int zmk_custom_setting_array_pop_back(const struct zmk_custom_setting *setting,
                                      struct zmk_custom_setting_value *value,
                                      enum zmk_custom_setting_write_mode mode);

/* Save the current memory value to persistent storage. */
int zmk_custom_setting_save(const struct zmk_custom_setting *setting);
/* Discard unsaved memory/temporary changes and restore the persistent/default value. */
int zmk_custom_setting_discard(const struct zmk_custom_setting *setting);
/* Erase the persistent value and restore the default value. */
int zmk_custom_setting_reset(const struct zmk_custom_setting *setting);
/* End a temporary override and reveal the current memory value again. */
int zmk_custom_setting_rollback_temporary(const struct zmk_custom_setting *setting);

/* Apply save to settings matching the optional subsystem/key/prefix filters. */
int zmk_custom_settings_save_scope(const char *custom_subsystem_id, const char *key,
                                   const char *key_prefix, uint32_t *affected_count);
/* Apply discard to settings matching the optional subsystem/key/prefix filters. */
int zmk_custom_settings_discard_scope(const char *custom_subsystem_id, const char *key,
                                      const char *key_prefix, uint32_t *affected_count);
/* Apply reset to settings matching the optional subsystem/key/prefix filters. */
int zmk_custom_settings_reset_scope(const char *custom_subsystem_id, const char *key,
                                    const char *key_prefix, uint32_t *affected_count);

bool zmk_custom_setting_has_unsaved_value(const struct zmk_custom_setting *setting);
/* Validate value type and all constraints for a setting without writing it. */
int zmk_custom_setting_validate(const struct zmk_custom_setting *setting,
                                const struct zmk_custom_setting_value *value);
