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
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR = 5,
};

/* A ZMK behavior binding: a behavior local ID (see CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
 * plus its two binding parameters. Writes are validated against the target
 * behavior's parameter metadata (see zmk_behavior_validate_binding in
 * <drivers/behavior.h>) when CONFIG_ZMK_BEHAVIOR_LOCAL_IDS is enabled. */
struct zmk_custom_setting_behavior_value {
    uint32_t behavior_id;
    uint32_t param1;
    uint32_t param2;
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
        struct zmk_custom_setting_behavior_value behavior_value;
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

/*
 * Mutable, per-array state for one array setting: a single contiguous
 * backing buffer plus per-slot bookkeeping, sized once by max_count instead
 * of being duplicated onto N separate struct zmk_custom_setting element
 * registrations (the pre-P3 design). Allocated statically by
 * ZMK_CUSTOM_SETTING_ARRAY_DEFINE (one instance per array) and referenced by
 * `array_state` from both the array's single registered descriptor and any
 * short-lived "index view" struct zmk_custom_setting instances that
 * zmk_custom_setting_find_array_element hands out (see array_view_pool in
 * custom_settings.c). All access happens under settings_lock.
 */
struct zmk_custom_setting_array_state {
    struct zmk_custom_setting_value *values;
    /* True if `values[i]`/`dirty[i]`/`has_persistent[i]` has ever been
     * written to (memory or persisted) since the last reset - lets save/
     * discard/reset operate in O(active count) without walking a registry. */
    bool *dirty;
    bool *has_persistent;
    uint32_t max_size;
    uint32_t default_size;
    uint32_t size;
    uint32_t persistent_size;
    const struct zmk_custom_setting_value *defaults;
};

struct zmk_custom_setting {
    const char *custom_subsystem_id;
    const char *key;
    const char *array_key;
    /* ZMK_CUSTOM_SETTING_ARRAY_NONE for scalars. For the one registered
     * array descriptor, also ZMK_CUSTOM_SETTING_ARRAY_NONE (the descriptor
     * itself does not represent one element). For an index view handed out
     * by zmk_custom_setting_find_array_element, the requested element
     * index. */
    uint32_t array_index;
    /* Non-NULL for both the array's descriptor and any index view of it;
     * NULL for scalars. This is the "back-pointer to the owning descriptor's
     * shared state" mentioned in the P3 design: rather than pointing at the
     * owning struct zmk_custom_setting (which would require an extra
     * indirection to reach the buffer), it points directly at the shared
     * mutable array_state, since that is all effective_value()/
     * write_value_locked()/etc. need to read or write element `array_index`. */
    struct zmk_custom_setting_array_state *array_state;
    enum zmk_custom_setting_value_type value_type;
    enum zmk_custom_setting_confidentiality confidentiality;
    enum zmk_custom_setting_permission read_permission;
    enum zmk_custom_setting_permission write_permission;
    const struct zmk_custom_setting_constraint *constraints;
    size_t constraints_count;
    /* Points at a static const object generated by the registration macros,
     * so the default lives in flash instead of being copied into this
     * (mutable, RAM-resident) struct. Unused (NULL) for the array descriptor
     * and its views; array element defaults live in array_state->defaults
     * instead, since each element has its own default. */
    const struct zmk_custom_setting_value *default_value;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;

    bool initialized;
    /* The following scalar-value bookkeeping fields are meaningful only for
     * non-array settings; array settings keep the equivalent per-element
     * state in array_state (has_persistent[i]/dirty[i]/values[i]) since a
     * single struct zmk_custom_setting no longer represents one element. */
    bool has_persistent_value;
    /* Set on any memory-mode write, cleared on save/discard/reset. Replaces
     * comparing memory_value against a RAM-resident copy of the persisted
     * value (removed; see zmk_custom_setting_discard) so
     * zmk_custom_setting_has_unsaved_value stays a cheap flag check even
     * though it runs on the Studio RPC list hot path. */
    bool dirty;
    bool temporary_active;
    /* Index into the shared temporary-override pool (see custom_settings.c),
     * or -1 when temporary_active is false. Avoids a full-size
     * struct zmk_custom_setting_value slot per setting for a mode that is
     * rarely used and never persisted. For an array element, this lives on
     * the index-view instance handed out for (array, index); the pool keys
     * views by (array_state, array_index) so the same view (and therefore
     * the same temp_slot) is reused across lookups for the same element -
     * see array_view_pool in custom_settings.c. */
    int8_t temp_slot;
    struct zmk_custom_setting_value memory_value;

    /*
     * Per-setting large-value backing store (issue #16). For a normal
     * setting these are all zero/NULL and the value lives in the fixed-size
     * `memory_value` union above (capped at
     * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE). A BYTES/STRING setting
     * registered with an explicit larger max_size via
     * ZMK_CUSTOM_SETTING_DEFINE_SIZED instead points `large_data` at a
     * right-sized static buffer of `value_max_size`(+1) bytes and stores its
     * payload there, so one large setting does not inflate every other
     * setting's footprint. `large_size` is the current payload length. Large
     * values are only reachable via the chunked ReadValueChunk/WriteValueChunk
     * RPC and zmk_custom_setting_read_into / zmk_custom_setting_write_bytes;
     * the fixed-carrier zmk_custom_setting_read/write and the single-frame
     * RPC report -EMSGSIZE / omit the value once it exceeds the carrier.
     *
     * `value_max_size == 0` means "use CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE"
     * so hand-built descriptors that leave these fields zero keep the legacy
     * behavior with no change.
     */
    uint32_t value_max_size;
    uint8_t *large_data;
    size_t large_size;

    /*
     * Intrusive singly-linked list pointer used only by
     * zmk_custom_settings_register() (P5a) for settings registered at
     * runtime instead of via the compile-time ZMK_CUSTOM_SETTING_DEFINE/
     * ZMK_CUSTOM_SETTING_ARRAY_DEFINE macros (STRUCT_SECTION_ITERABLE).
     * NULL for every compile-time-registered setting; ZMK_CUSTOM_SETTING_FOREACH
     * walks the compile-time section followed by this list, so callers do
     * not need to distinguish the two registration styles. Left as a plain
     * struct field (rather than a private companion table) so registration
     * stays heap-free: the caller's own static storage for the descriptor
     * is threaded directly into the list. */
    struct zmk_custom_setting *_runtime_next;
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

#define ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(_behavior_id, _param1, _param2)                          \
    ((struct zmk_custom_setting_value){                                                            \
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,                                            \
        .behavior_value = {.behavior_id = (_behavior_id),                                          \
                           .param1 = (_param1),                                                    \
                           .param2 = (_param2)},                                                   \
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

/*
 * Register one BYTES/STRING setting able to store a value up to _max_size
 * bytes - larger than the fixed CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE
 * carrier (issue #16). _max_size must not exceed
 * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE. A right-sized static
 * backing buffer is allocated only for this setting, so other settings keep
 * their small footprint. A value larger than
 * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE can only be read/written through
 * the chunked ReadValueChunk/WriteValueChunk Studio RPC and the
 * zmk_custom_setting_read_into / zmk_custom_setting_write_bytes firmware API;
 * the fixed-carrier zmk_custom_setting_read/write and the single-frame RPC
 * report -EMSGSIZE / omit the value once it exceeds the carrier.
 */
#define ZMK_CUSTOM_SETTING_DEFINE_SIZED(_name, _max_size, _custom_subsystem_id, _key, _value_type, \
                                        _default_value, _confidentiality, _read_permission,        \
                                        _write_permission, _constraint)                            \
    ZMK_CUSTOM_SETTING_DEFINE_SIZED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                           \
        _name, _max_size, _custom_subsystem_id, _key, _value_type, _default_value,                 \
        _confidentiality, _read_permission, _write_permission, NULL, NULL, _constraint)

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
    ZMK_CUSTOM_SETTING_DEFINE_SIZED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                           \
        _name, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE, _custom_subsystem_id, _key, _value_type, \
        _default_value, _confidentiality, _read_permission, _write_permission, _rpc_serializer,    \
        _rpc_deserializer, __VA_ARGS__)

/*
 * Innermost sized registration macro. Allocates a right-sized static backing
 * buffer ONLY when _max_size exceeds the fixed carrier size, so a normal
 * setting (default max_size) keeps large_data == NULL and its legacy
 * union-based storage. The ternary in the array size and the .large_data
 * initializer are both constant expressions (both operands compile-time
 * constants), so this stays a valid static initializer.
 */
#define ZMK_CUSTOM_SETTING_DEFINE_SIZED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                       \
    _name, _max_size, _custom_subsystem_id, _key, _value_type, _default_value, _confidentiality,   \
    _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, ...)                  \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT(sizeof(_key) <= CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                           \
                 "Custom setting key is too long");                                                \
    BUILD_ASSERT((_max_size) <= CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,                   \
                 "Custom setting max_size exceeds "                                                \
                 "CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE");                               \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    static const struct zmk_custom_setting_value _name##_default = _default_value;                 \
    static uint8_t _name##_large_data                                                              \
        [((_max_size) > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? (_max_size) : 0) + 1];         \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key = _key,                                                                               \
        .array_key = NULL,                                                                         \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .array_state = NULL,                                                                       \
        .value_type = _value_type,                                                                 \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = &_name##_default,                                                         \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
        .temp_slot = -1,                                                                           \
        .value_max_size = (_max_size),                                                             \
        .large_data =                                                                              \
            ((_max_size) > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? _name##_large_data : NULL), \
    }

/*
 * ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE (and its _WITH_CONSTRAINTS /
 * _WITH_RPC_CONVERTERS / _WITH_RPC_CONVERTERS_AND_CONSTRAINTS variants) was
 * removed in the P3 rework. It used to register one full
 * STRUCT_SECTION_ITERABLE(zmk_custom_setting, ...) descriptor PER ARRAY
 * ELEMENT (so an N-element array meant N independent registrations, each
 * duplicating array_max_size/array_size/persistent_array_size and each
 * carrying its own full-size memory_value), and relied on
 * ZMK_CUSTOM_SETTING_FOREACH walks at runtime to find "sibling" elements of
 * the same array to keep that duplicated bookkeeping in sync - the O(N)
 * memory duplication and O(N)/O(N^2) iteration cost this rework eliminates.
 *
 * That per-element registration cannot be preserved as a compatibility
 * shim: it was designed to be expanded N times by independent callers (e.g.
 * zmk-feature-runtime-macro's LISTIFY(CONFIG_ZMK_RUNTIME_MACRO_COUNT, ...)
 * pattern), and N independent preprocessor macro invocations have no way to
 * share state with each other, so they cannot cooperate to build the one
 * shared descriptor + contiguous buffer this rework requires. Only a
 * wrapping "define the whole array at once" macro can do that, which is a
 * breaking API change for any caller that expanded the old macro in a loop.
 *
 * Replace each call site with one ZMK_CUSTOM_SETTING_ARRAY_DEFINE
 * registration for the whole array (see below). This module's own tests and
 * samples have been migrated; zmk-feature-runtime-macro (out of scope for
 * this change) will need the same migration before it can build against
 * this version of the header.
 */

/* Register an array setting: one descriptor owning a single contiguous
 * backing buffer for up to _max_count elements, replacing
 * ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE's one-registration-per-element
 * design (see removal note above). _defaults must be a pointer to a
 * separately-declared `static const struct zmk_custom_setting_value[]` of
 * length _max_count (see ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE) -
 * a pointer, not a compound literal, for the same reason
 * ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32 takes a constraint pointer (nesting
 * a compound literal here breaks strict -std=c11 constant-initializer
 * rules). _default_size is the active length new/reset arrays start with
 * (<= _max_count). */
#define ZMK_CUSTOM_SETTING_ARRAY_DEFINE(_name, _custom_subsystem_id, _key, _value_type,            \
                                        _max_count, _default_size, _defaults, _confidentiality,    \
                                        _read_permission, _write_permission, _constraint)          \
    ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_CONSTRAINTS(                                              \
        _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,      \
        _confidentiality, _read_permission, _write_permission, _constraint)

/* Register an array setting with custom bytes RPC conversion hooks. */
#define ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS(                                       \
    _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,          \
    _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer,     \
    _constraint)                                                                                   \
    ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                           \
        _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,      \
        _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer, \
        _constraint)

#define ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_CONSTRAINTS(                                          \
    _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,          \
    _confidentiality, _read_permission, _write_permission, ...)                                    \
    ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                           \
        _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,      \
        _confidentiality, _read_permission, _write_permission, NULL, NULL, __VA_ARGS__)

#define ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                       \
    _name, _custom_subsystem_id, _key, _value_type, _max_count, _default_size, _defaults,          \
    _confidentiality, _read_permission, _write_permission, _rpc_serializer, _rpc_deserializer,     \
    ...)                                                                                           \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT(sizeof(_key) <= CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN,                           \
                 "Custom setting array key is too long");                                          \
    BUILD_ASSERT((_default_size) <= (_max_count),                                                  \
                 "Custom setting array default size must not exceed max count");                   \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    static struct zmk_custom_setting_value _name##_values[_max_count];                             \
    static bool _name##_dirty[_max_count];                                                         \
    static bool _name##_has_persistent[_max_count];                                                \
    static struct zmk_custom_setting_array_state _name##_array_state = {                           \
        .values = _name##_values,                                                                  \
        .dirty = _name##_dirty,                                                                    \
        .has_persistent = _name##_has_persistent,                                                  \
        .max_size = (_max_count),                                                                  \
        .default_size = (_default_size),                                                           \
        .defaults = (_defaults),                                                                   \
    };                                                                                             \
    STRUCT_SECTION_ITERABLE(zmk_custom_setting, _name) = {                                         \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key = _key,                                                                               \
        .array_key = _key,                                                                         \
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,                                              \
        .array_state = &_name##_array_state,                                                       \
        .value_type = _value_type,                                                                 \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = NULL,                                                                     \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
        .temp_slot = -1,                                                                           \
    }

/* Declares a `static const struct zmk_custom_setting_value[]` of INT32
 * defaults suitable for ZMK_CUSTOM_SETTING_ARRAY_DEFINE's _defaults
 * argument. Field-initializer syntax (not the ZMK_CUSTOM_SETTING_VALUE_INT32
 * compound literal) is used for the same reason as
 * ZMK_CUSTOM_SETTING_RANGE_INT32: nesting a compound literal inside this
 * array's own initializer breaks strict -std=c11 constant-initializer
 * rules. */
#define ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(_name, ...)                                  \
    static const struct zmk_custom_setting_value _name[] = {                                       \
        ZMK_CUSTOM_SETTINGS_INT32_LIST(__VA_ARGS__)}

#define ZMK_CUSTOM_SETTINGS_INT32_LIST(...)                                                        \
    FOR_EACH(ZMK_CUSTOM_SETTINGS_INT32_LIST_ITEM, (, ), __VA_ARGS__)
#define ZMK_CUSTOM_SETTINGS_INT32_LIST_ITEM(_value)                                                \
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_value)}

/*
 * Iterate every registered setting: compile-time (STRUCT_SECTION_ITERABLE,
 * via ZMK_CUSTOM_SETTING_DEFINE/ZMK_CUSTOM_SETTING_ARRAY_DEFINE) entries
 * first, then any settings added at runtime via
 * zmk_custom_settings_register() (P5a). Every call site that previously
 * assumed ZMK_CUSTOM_SETTING_FOREACH == STRUCT_SECTION_FOREACH (find,
 * find_array, apply_scope, the Studio RPC list handler,
 * custom_settings_init, scope_has_permission) picks up runtime-registered
 * settings for free through this macro instead of needing its own
 * modification.
 *
 * Implemented as a virtual-iterator function pair rather than trying to
 * splice two independent `for` loops (one over the linker section, one over
 * the runtime list) so that a single shared loop variable and trailing
 * `{ body }` block works the same way STRUCT_SECTION_FOREACH's callers
 * already expect.
 */
#define ZMK_CUSTOM_SETTING_FOREACH(_var)                                                           \
    for (const struct zmk_custom_setting *_var = zmk_custom_settings_foreach_first();              \
         _var != NULL; _var = zmk_custom_settings_foreach_next(_var))

/* Iterator internals for ZMK_CUSTOM_SETTING_FOREACH; not intended to be
 * called directly. */
const struct zmk_custom_setting *zmk_custom_settings_foreach_first(void);
const struct zmk_custom_setting *
zmk_custom_settings_foreach_next(const struct zmk_custom_setting *current);

/*
 * Register a setting at runtime instead of through the compile-time
 * ZMK_CUSTOM_SETTING_DEFINE/ZMK_CUSTOM_SETTING_ARRAY_DEFINE macros (P5a) -
 * for drivers whose setting count depends on devicetree instance data or
 * other runtime probing that macros can't express. `desc` must point at
 * caller-owned storage (typically static/BSS) that outlives the
 * registration; only the pointer is stored, so this stays heap-free. The
 * descriptor's `_runtime_next` field must be zero (e.g. a plain
 * `static struct zmk_custom_setting my_setting = {...};` with `_runtime_next`
 * left unset) - it is used internally to link registered settings together
 * and must not be touched by the caller afterwards.
 *
 * Initializes the descriptor's mutable state (memory_value from
 * default_value, has_persistent_value/dirty/temp_slot/etc.) the same way
 * custom_settings_init() does for compile-time settings, then - since
 * registration can happen after this module's own settings_load() already
 * ran at boot - loads any already-persisted value for this setting via
 * settings_load_subtree() so it does not silently fall back to the default
 * for the rest of this boot. Returns -EINVAL for a NULL/malformed
 * descriptor, -EEXIST if a setting with the same subsystem id + key (or
 * array key) is already registered.
 */
int zmk_custom_settings_register(struct zmk_custom_setting *desc);

/*
 * Unregister a setting previously added by zmk_custom_settings_register()
 * (P5b: releases a keyspace slot back to its pool via
 * zmk_custom_setting_keyspace_delete). Not meaningful for compile-time
 * (STRUCT_SECTION_ITERABLE) settings - only settings whose `_runtime_next`
 * was set by zmk_custom_settings_register() can be unlinked this way.
 * Returns -ENOENT if `desc` is not currently registered.
 */
int zmk_custom_settings_unregister(struct zmk_custom_setting *desc);

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
/* Insert one value at index, shifting existing elements at and after index
 * one slot later (memmove over the array's single contiguous buffer) and
 * growing the active length by one. index == current array size behaves
 * like array_push_back. Fails with -ERANGE if the array is already at its
 * maximum size or index is past the (post-insert) end. */
int zmk_custom_setting_array_insert_at(const struct zmk_custom_setting *array_setting_or_element,
                                       uint32_t index, const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode);
/* Remove the value at index, shifting later elements one slot earlier
 * (memmove) and shrinking the active length by one. Optionally returns the
 * removed value. Fails with -ENOENT if index is past the active length. */
int zmk_custom_setting_array_remove_at(const struct zmk_custom_setting *array_setting_or_element,
                                       uint32_t index, struct zmk_custom_setting_value *out_value,
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

/*
 * Replace a setting's default value with one resolved at boot time (e.g. from
 * devicetree data that needs behavior local IDs or other runtime-only state
 * to encode). `value` must point to storage that outlives the setting -
 * typically a caller-owned static/BSS-resident struct - since only the
 * pointer is stored, not a copy.
 *
 * Also refreshes the effective (memory) value to match, but only if no
 * persistent value has been loaded and no write has happened yet - so this
 * is safe to call from any SYS_INIT regardless of ordering relative to this
 * module's own registry init, as long as it runs before settings_load()
 * (which always happens later, from main()). Once a persisted or written
 * value exists, this only changes what a later save/discard/reset falls
 * back to.
 */
int zmk_custom_setting_set_default(const struct zmk_custom_setting *setting,
                                   const struct zmk_custom_setting_value *value);

/*
 * View-based read/write API.
 *
 * These avoid requiring callers to declare a full struct
 * zmk_custom_setting_value (sized by CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE)
 * on their own stack just to read or write one small or dynamically-sized
 * value, and let bytes/string values be written from a runtime buffer
 * instead of only the compile-time-literal ZMK_CUSTOM_SETTING_VALUE_BYTES(...)
 * macro.
 */

/* Callback invoked with the effective value while the settings lock is held.
 * Runs synchronously; keep it short, copy out anything needed, and do not
 * call back into other zmk_custom_setting_* functions for the same or other
 * settings from within it. */
typedef void (*zmk_custom_setting_value_visitor_t)(const struct zmk_custom_setting_value *value,
                                                   void *user_data);

/* Borrow the effective value without copying it into a caller-owned
 * struct zmk_custom_setting_value first. */
int zmk_custom_setting_with_value(const struct zmk_custom_setting *setting,
                                  zmk_custom_setting_value_visitor_t visitor, void *user_data);

/* Copy the effective value's raw payload into a caller-sized buffer.
 * Returns -EMSGSIZE if capacity is smaller than the value. out_size and
 * out_type may be NULL. */
int zmk_custom_setting_read_into(const struct zmk_custom_setting *setting, void *buf,
                                 size_t capacity, size_t *out_size,
                                 enum zmk_custom_setting_value_type *out_type);

/* Write a BYTES or STRING setting from a runtime buffer instead of a
 * compile-time-literal value. For STRING settings, size is measured before
 * any truncation to CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE. */
int zmk_custom_setting_write_bytes(const struct zmk_custom_setting *setting, const void *data,
                                   size_t size, enum zmk_custom_setting_write_mode mode);

/* Typed scalar convenience wrappers over read_into/write. Return -EINVAL if
 * the setting is not of the matching type. */
int zmk_custom_setting_get_int32(const struct zmk_custom_setting *setting, int32_t *value);
int zmk_custom_setting_set_int32(const struct zmk_custom_setting *setting, int32_t value,
                                 enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_get_bool(const struct zmk_custom_setting *setting, bool *value);
int zmk_custom_setting_set_bool(const struct zmk_custom_setting *setting, bool value,
                                enum zmk_custom_setting_write_mode mode);

/* Get/set a BEHAVIOR setting. set validates behaviorId/param1/param2 against
 * the target behavior's parameter metadata (see zmk_custom_setting_validate). */
int zmk_custom_setting_get_behavior(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_behavior_value *value);
int zmk_custom_setting_set_behavior(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_behavior_value value,
                                    enum zmk_custom_setting_write_mode mode);

/*
 * Record settings: a struct with a declared field schema, stored as a
 * versioned TLV (field id, length, value) encoding inside a BYTES setting.
 * Unknown field ids are skipped on decode and fields absent from the stored
 * data are left untouched in the destination struct (so callers should
 * populate defaults before calling zmk_custom_setting_record_get); adding a
 * field to the struct does not invalidate previously stored data.
 *
 * This builds on the existing BYTES value type and zmk_custom_setting_write_bytes/
 * read_into; there is no new RPC value type, so a record setting's RPC/export
 * representation is the encoded TLV bytes. zmk_custom_setting_record_set only
 * enforces ZMK_CUSTOM_SETTING_CONSTRAINT_NONE and
 * ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE (on INT32 fields); other constraint
 * kinds fail with -ENOTSUP.
 */
#define ZMK_CUSTOM_SETTING_RECORD_VERSION 1
#define ZMK_CUSTOM_SETTING_RECORD_MAX_FIELD_SIZE 255

struct zmk_custom_setting_record_field {
    uint8_t field_id;
    size_t offset;
    size_t size;
    enum zmk_custom_setting_value_type type;
    const struct zmk_custom_setting_constraint *constraint;
};

struct zmk_custom_setting_record_schema {
    const struct zmk_custom_setting_record_field *fields;
    size_t fields_count;
    size_t record_size;
};

/* _constraint is a pointer to a separately-declared static const
 * struct zmk_custom_setting_constraint (see
 * ZMK_CUSTOM_SETTING_RECORD_RANGE_INT32_DEFINE), or NULL for no constraint.
 * It must be a pointer, not the ZMK_CUSTOM_SETTING_RANGE_INT32(...) compound
 * literal used elsewhere: nesting that inside this field's own initializer
 * hits the same non-constant-expression restriction that
 * ZMK_CUSTOM_SETTING_RANGE_INT32's own definition works around. */
#define ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32(_struct, _field, _id, _constraint)                   \
    {.field_id = (_id),                                                                            \
     .offset = offsetof(_struct, _field),                                                          \
     .size = sizeof(((_struct *)0)->_field),                                                       \
     .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,                                                  \
     .constraint = (_constraint)}

/* Declares a named, reusable range constraint suitable for
 * ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32's _constraint argument (pass
 * &_name). Uses field initializers rather than the
 * ZMK_CUSTOM_SETTING_RANGE_INT32(...) compound literal so the declaration
 * itself is a valid constant initializer. */
#define ZMK_CUSTOM_SETTING_RECORD_RANGE_INT32_DEFINE(_name, _min, _max)                            \
    static const struct zmk_custom_setting_constraint _name = {                                    \
        .type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,                                               \
        .range = {.min = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_min)},     \
                  .max = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = (_max)}}}

#define ZMK_CUSTOM_SETTING_RECORD_FIELD_BOOL(_struct, _field, _id)                                 \
    {.field_id = (_id),                                                                            \
     .offset = offsetof(_struct, _field),                                                          \
     .size = sizeof(((_struct *)0)->_field),                                                       \
     .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,                                                   \
     .constraint = NULL}

/* _field must be a fixed-size char array; the encoded/decoded length always
 * leaves room for a NUL terminator within that array. */
#define ZMK_CUSTOM_SETTING_RECORD_FIELD_STRING(_struct, _field, _id)                               \
    {.field_id = (_id),                                                                            \
     .offset = offsetof(_struct, _field),                                                          \
     .size = sizeof(((_struct *)0)->_field),                                                       \
     .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,                                                 \
     .constraint = NULL}

/* Declares the fields array and schema object as two plain (non-compound-
 * literal) static objects -- required so this expands to a valid constant
 * initializer under strict -std=c11 (see ZMK_CUSTOM_SETTING_RANGE_INT32). */
#define ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE(_name, _struct, ...)                               \
    static const struct zmk_custom_setting_record_field _name##_fields[] = {__VA_ARGS__};          \
    static const struct zmk_custom_setting_record_schema _name = {                                 \
        .fields = _name##_fields,                                                                  \
        .fields_count = ARRAY_SIZE(_name##_fields),                                                \
        .record_size = sizeof(_struct),                                                            \
    }

/* Encode record (a pointer to a struct matching schema) into a TLV byte
 * buffer. Returns -EMSGSIZE if it does not fit out_capacity or a field
 * exceeds ZMK_CUSTOM_SETTING_RECORD_MAX_FIELD_SIZE bytes. */
int zmk_custom_setting_record_encode(const struct zmk_custom_setting_record_schema *schema,
                                     const void *record, uint8_t *out, size_t out_capacity,
                                     size_t *out_size);

/* Decode a TLV byte buffer into record. Fields present in the data but not
 * in schema are skipped; fields in schema but absent from the data are left
 * unmodified in record. Returns -ENOTSUP for an unrecognized version byte. */
int zmk_custom_setting_record_decode(const struct zmk_custom_setting_record_schema *schema,
                                     const uint8_t *data, size_t data_size, void *record);

/* Validate record's fields against schema's constraints, then encode and
 * write it to a BYTES setting via zmk_custom_setting_write_bytes. */
int zmk_custom_setting_record_set(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_setting_record_schema *schema,
                                  const void *record, enum zmk_custom_setting_write_mode mode);

/* Read a BYTES setting's stored TLV bytes and decode them into record.
 * Populate record with defaults before calling, since fields absent from
 * the stored data are left as-is. */
int zmk_custom_setting_record_get(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_setting_record_schema *schema,
                                  void *record);

/*
 * P5b: RPC-creatable keyspaces.
 *
 * A keyspace lets a module accept user-created entries (profiles, named
 * mappings, ...) whose keys are not known at compile time, without using the
 * heap: ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE statically allocates a fixed pool
 * of `max_entries` slots, each with its own `max_key_len`-sized key buffer
 * (independent of CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN, which only bounds
 * lookup/matching scratch buffers - see zmk_custom_setting_matches). Entries
 * are created/deleted at runtime via zmk_custom_setting_keyspace_create /
 * zmk_custom_setting_keyspace_delete (wrapped by the Studio
 * CreateSetting/DeleteSetting RPC requests), each of which builds a
 * `struct zmk_custom_setting` for the slot and threads it into the same
 * runtime registration list as zmk_custom_settings_register() (P5a) - so a
 * created entry is automatically find-able, listable (via the existing
 * key_prefix scope filter and ZMK_CUSTOM_SETTING_FOREACH), and covered by
 * save/discard/reset with zero further call-site changes, the same leverage
 * point P5a already established.
 *
 * Persisted entries are re-bound to a slot during settings_load(): see
 * zmk_custom_setting_keyspace_bind, called from the SETTINGS_STATIC_HANDLER
 * callback in custom_settings.c for any "<subsystem>/<key>" record whose key
 * matches a registered keyspace's prefix but has no live slot yet, so a
 * previously created entry survives reboot without any module code running
 * on its behalf.
 */

struct zmk_custom_setting_keyspace_slot {
    bool in_use;
    struct zmk_custom_setting setting;
};

/*
 * Fixed-size, statically-allocated keyspace state: one `max_entries`-slot
 * pool plus one `max_entries x max_key_len` key-storage table, both declared
 * by ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE and referenced by pointer so the
 * keyspace descriptor itself stays a small, constant, read-only object.
 */
struct zmk_custom_setting_keyspace {
    const char *custom_subsystem_id;
    /* Every created entry's key must start with this prefix, e.g. "macro/".
     * Also the prefix advertised for discovery via the existing key_prefix
     * scope filter. */
    const char *key_prefix;
    enum zmk_custom_setting_value_type value_type;
    /* Per-keyspace value size ceiling. May be smaller than the global
     * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE (enforced as a tighter limit
     * via zmk_custom_setting_keyspace_create / the settings-load bind path)
     * or, for BYTES/STRING keyspaces, LARGER than it (up to
     * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE) - in which case each
     * slot gets its own large_data row from `value_bufs` and entries transfer
     * over the chunked RPC, exactly like a ZMK_CUSTOM_SETTING_DEFINE_SIZED
     * setting (issue #16). */
    uint32_t max_size;
    /* Size of each slot's key buffer, INCLUDING the NUL terminator - a
     * per-keyspace macro argument instead of the global
     * CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN, so a keyspace for short
     * suffixes (e.g. "macro/<name>") does not pay for one sized for the
     * global maximum. */
    uint32_t max_key_len;
    uint32_t max_entries;
    enum zmk_custom_setting_confidentiality confidentiality;
    enum zmk_custom_setting_permission read_permission;
    enum zmk_custom_setting_permission write_permission;
    const struct zmk_custom_setting_constraint *constraints;
    size_t constraints_count;
    const struct zmk_custom_setting_value *default_value;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;

    struct zmk_custom_setting_keyspace_slot *slots;
    char *keys; /* max_entries * max_key_len bytes, row-major. */
    /* Per-slot large-value backing store, row-major
     * (max_entries * value_buf_stride bytes), or NULL when max_size fits the
     * fixed CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE carrier (issue #16).
     * When present, each slot's struct zmk_custom_setting.large_data points at
     * its own row so a keyspace entry can hold a value larger than the
     * carrier, transferred over the chunked RPC like any other large
     * setting. */
    uint8_t *value_bufs;
    uint32_t value_buf_stride; /* bytes per slot row, = max_size + 1 */

    /* Intrusive singly-linked list pointer, analogous to
     * struct zmk_custom_setting's _runtime_next; used internally by
     * zmk_custom_settings_register_keyspace and
     * ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH. Must be zero at registration
     * time (the macro-generated static object leaves it unset) and must
     * not be touched by the caller afterwards. */
    struct zmk_custom_setting_keyspace *_next;
};

/* Register a keyspace. Call once at boot (e.g. from a SYS_INIT), before
 * settings_load() runs, so persisted entries under this keyspace's prefix
 * are re-bound as settings_load() encounters them - see
 * zmk_custom_setting_keyspace_bind. Returns -EINVAL for a malformed
 * keyspace, -EEXIST if the same subsystem id + key_prefix is already
 * registered. */
int zmk_custom_settings_register_keyspace(struct zmk_custom_setting_keyspace *keyspace);

/* Create one entry: `key` must start with keyspace->key_prefix and fit in
 * max_key_len (including NUL). Allocates a free slot, registers it (so it is
 * immediately find-able/listable), and writes `value` to it in `mode`.
 * Returns -ENOSPC if every slot is in use, -EEXIST if `key` already has a
 * live slot, -ENAMETOOLONG if `key` does not fit, -EINVAL for a mismatched
 * prefix/value type. On success, `*out_setting` (if non-NULL) receives the
 * new entry's descriptor. */
int zmk_custom_setting_keyspace_create(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode,
                                       const struct zmk_custom_setting **out_setting);

/* Delete one entry by key: erases its persisted storage record (if any) and
 * releases its slot back to the pool for reuse. Returns -ENOENT if `key` has
 * no live slot in this keyspace. */
int zmk_custom_setting_keyspace_delete(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key);

/* Find a keyspace's currently live entry by its literal key (e.g.
 * "macro/my-macro-1"), or NULL if not bound to a slot right now. */
const struct zmk_custom_setting *
zmk_custom_setting_keyspace_find(const struct zmk_custom_setting_keyspace *keyspace,
                                 const char *key);

/* Find the registered keyspace (if any) whose custom_subsystem_id matches
 * and whose key_prefix is a prefix of `key` - used by the Studio RPC
 * CreateSetting/DeleteSetting handlers to resolve a request's plain
 * (subsystem, key) pair to the keyspace that owns it, without the caller
 * needing its own pointer to the struct zmk_custom_setting_keyspace. */
struct zmk_custom_setting_keyspace *
zmk_custom_settings_keyspace_find_for_key(const char *custom_subsystem_id, const char *key);

/*
 * Internal: bind an unclaimed slot to `key` (which must match this
 * keyspace's prefix) without writing any value - used only by the
 * SETTINGS_STATIC_HANDLER_DEFINE callback in custom_settings.c while
 * settings_load() is iterating persisted records, immediately before the
 * loaded value is applied to the returned descriptor via the normal
 * value_from_storage path. Not intended to be called from module code:
 * use zmk_custom_setting_keyspace_create for that. Returns NULL (silently,
 * dropping the record) if the pool is exhausted - a reboot cannot fail, so
 * an over-quota persisted entry is skipped rather than blocking boot.
 */
struct zmk_custom_setting *
zmk_custom_setting_keyspace_bind_locked(struct zmk_custom_setting_keyspace *keyspace,
                                        const char *key);

/* Iterate every registered keyspace (compile-time or otherwise; currently
 * only registered via zmk_custom_settings_register_keyspace, but exposed as
 * an iterator for symmetry with ZMK_CUSTOM_SETTING_FOREACH and to let the
 * settings-load hook find "does this key match any keyspace" without a
 * separate registry). Internal to this module + its Studio RPC handler. */
#define ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(_var)                                                  \
    for (struct zmk_custom_setting_keyspace *_var = zmk_custom_settings_keyspace_foreach_first();  \
         _var != NULL; _var = zmk_custom_settings_keyspace_foreach_next(_var))

struct zmk_custom_setting_keyspace *zmk_custom_settings_keyspace_foreach_first(void);
struct zmk_custom_setting_keyspace *
zmk_custom_settings_keyspace_foreach_next(struct zmk_custom_setting_keyspace *current);

/* Register a keyspace with a fixed-size static slot pool and key buffer
 * table. _max_key_len must include room for the NUL terminator.
 * _default_value is used as every newly bound/created slot's default (the
 * same static const object is shared by every slot, matching how a scalar
 * ZMK_CUSTOM_SETTING_DEFINE setting's default_value works). */
#define ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(                                                        \
    _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len, _max_entries,  \
    _default_value, _confidentiality, _read_permission, _write_permission, _constraint)            \
    ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_CONSTRAINTS(                                           \
        _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len,            \
        _max_entries, _default_value, _confidentiality, _read_permission, _write_permission,       \
        _constraint)

#define ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_CONSTRAINTS(                                       \
    _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len, _max_entries,  \
    _default_value, _confidentiality, _read_permission, _write_permission, ...)                    \
    ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                        \
        _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len,            \
        _max_entries, _default_value, _confidentiality, _read_permission, _write_permission, NULL, \
        NULL, __VA_ARGS__)

#define ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_RPC_CONVERTERS_AND_CONSTRAINTS(                    \
    _name, _custom_subsystem_id, _key_prefix, _value_type, _max_size, _max_key_len, _max_entries,  \
    _default_value, _confidentiality, _read_permission, _write_permission, _rpc_serializer,        \
    _rpc_deserializer, ...)                                                                        \
    BUILD_ASSERT(sizeof(_custom_subsystem_id) <=                                                   \
                     CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN,                       \
                 "Custom subsystem id is too long");                                               \
    BUILD_ASSERT((_max_size) <= CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,                   \
                 "Keyspace max_size exceeds CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE");     \
    BUILD_ASSERT((_max_key_len) > sizeof(_key_prefix),                                             \
                 "Keyspace max_key_len must leave room for at least one suffix byte plus NUL "     \
                 "after the prefix");                                                              \
    static const struct zmk_custom_setting_constraint _name##_constraints[] = {__VA_ARGS__};       \
    static const struct zmk_custom_setting_value _name##_default = _default_value;                 \
    static struct zmk_custom_setting_keyspace_slot _name##_slots[_max_entries];                    \
    static char _name##_keys[_max_entries][_max_key_len];                                          \
    static uint8_t _name##_value_bufs                                                              \
        [_max_entries]                                                                             \
        [((_max_size) > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? (_max_size) : 0) + 1];         \
    static struct zmk_custom_setting_keyspace _name = {                                            \
        .custom_subsystem_id = _custom_subsystem_id,                                               \
        .key_prefix = _key_prefix,                                                                 \
        .value_type = _value_type,                                                                 \
        .max_size = (_max_size),                                                                   \
        .max_key_len = (_max_key_len),                                                             \
        .max_entries = (_max_entries),                                                             \
        .confidentiality = _confidentiality,                                                       \
        .read_permission = _read_permission,                                                       \
        .write_permission = _write_permission,                                                     \
        .constraints = _name##_constraints,                                                        \
        .constraints_count = ARRAY_SIZE(_name##_constraints),                                      \
        .default_value = &_name##_default,                                                         \
        .rpc_serializer = _rpc_serializer,                                                         \
        .rpc_deserializer = _rpc_deserializer,                                                     \
        .slots = _name##_slots,                                                                    \
        .keys = &_name##_keys[0][0],                                                               \
        .value_bufs =                                                                              \
            ((_max_size) > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? &_name##_value_bufs[0][0]   \
                                                                     : NULL),                      \
        .value_buf_stride =                                                                        \
            ((_max_size) > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? (_max_size) + 1 : 0),       \
    }
