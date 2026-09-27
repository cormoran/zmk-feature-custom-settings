/* SPDX-License-Identifier: MIT */
#pragma once
/* Included by custom_settings.h after the value/state types. This layout retains
 * the fields accessed directly by older consumers. */
struct zmk_custom_setting_metadata {
    const struct zmk_custom_setting_constraint *constraints;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;
};

struct zmk_custom_setting {
    const char *custom_subsystem_id;
    const char *key;
    const char *array_key;
    uint32_t array_index; /* ARRAY_NONE on registered parents; index on scoped views. */
    uint8_t value_type;
    uint8_t confidentiality;
    uint8_t read_permission : 4, write_permission : 4;
    uint8_t constraints_count;
    const struct zmk_custom_setting_constraint *constraints;
    const struct zmk_custom_setting_value *default_value;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;
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

#define ZMK_CUSTOM_SETTING_ARRAY_KEY_INIT(key_) .array_key = (key_),
#define ZMK_CUSTOM_SETTING_METADATA_DEFINE(name_, ser_, des_)
#define ZMK_CUSTOM_SETTING_METADATA_INIT(name_, ser_, des_)                                        \
    .constraints = name_##_constraints, .rpc_serializer = (ser_), .rpc_deserializer = (des_),

static inline const struct zmk_custom_setting_constraint *
zmk_custom_setting_constraints(const struct zmk_custom_setting *setting) {
    return setting->constraints;
}
static inline zmk_custom_setting_rpc_bytes_converter_t
zmk_custom_setting_rpc_serializer(const struct zmk_custom_setting *setting) {
    return setting->rpc_serializer;
}
static inline zmk_custom_setting_rpc_bytes_converter_t
zmk_custom_setting_rpc_deserializer(const struct zmk_custom_setting *setting) {
    return setting->rpc_deserializer;
}
