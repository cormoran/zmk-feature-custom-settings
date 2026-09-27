/* SPDX-License-Identifier: MIT */
#pragma once
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
#include <cormoran/zmk/custom_settings/compat/descriptor.h>
#else
/* Included by custom_settings.h after the value/state types. Cold metadata is
 * shared by scoped array views; keyspace slots use the owner's metadata. */
struct zmk_custom_setting_metadata {
    const struct zmk_custom_setting_constraint *constraints;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;
};

struct zmk_custom_setting {
    const char *custom_subsystem_id;
    const char *key;
    uint32_t array_index; /* ARRAY_NONE on registered parents; index on scoped views. */
    uint8_t value_type;
    uint8_t confidentiality : 2, is_array : 1;
    uint8_t read_permission : 4, write_permission : 4;
    uint8_t constraints_count;
    const struct zmk_custom_setting_metadata *metadata;
    const struct zmk_custom_setting_value *default_value;
    union {
        struct {
            uint32_t max_size;
            struct zmk_custom_setting_large_pool *pool;
        } blob;
        struct zmk_custom_setting_array_state *array_state;
    };
    const struct zmk_custom_setting_keyspace *_keyspace;
    struct zmk_custom_setting_state *state;
};

#define ZMK_CUSTOM_SETTING_ARRAY_KEY_INIT(key_) .is_array = ((key_) != NULL),
#define ZMK_CUSTOM_SETTING_METADATA_DEFINE(name_, ser_, des_)                                      \
    static const struct zmk_custom_setting_metadata name_##_metadata = {                           \
        .constraints = name_##_constraints, .rpc_serializer = (ser_), .rpc_deserializer = (des_)}
#define ZMK_CUSTOM_SETTING_METADATA_INIT(name_, ser_, des_) .metadata = &name_##_metadata,

static inline const struct zmk_custom_setting_constraint *
zmk_custom_setting_constraints(const struct zmk_custom_setting *setting) {
    return setting->metadata ? setting->metadata->constraints : NULL;
}
static inline zmk_custom_setting_rpc_bytes_converter_t
zmk_custom_setting_rpc_serializer(const struct zmk_custom_setting *setting) {
    return setting->metadata ? setting->metadata->rpc_serializer : NULL;
}
static inline zmk_custom_setting_rpc_bytes_converter_t
zmk_custom_setting_rpc_deserializer(const struct zmk_custom_setting *setting) {
    return setting->metadata ? setting->metadata->rpc_deserializer : NULL;
}

#endif
