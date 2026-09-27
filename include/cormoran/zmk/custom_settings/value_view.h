/* SPDX-License-Identifier: MIT */
#pragma once
/* Included after custom_settings.h's descriptor and callback declarations.
 * Inputs borrow storage until the function returns. Copying outputs (including converters)
 * must be initialized with VIEW_BUFFER; INT32/BOOL-only outputs may use {0}.
 * Insufficient capacity fails before removing an array element. */
typedef void (*zmk_custom_setting_view_visitor_t)(const struct zmk_custom_setting_value_view *,
                                                  void *);
int zmk_custom_setting_read_view(const struct zmk_custom_setting *setting,
                                 struct zmk_custom_setting_value_view *value);
int zmk_custom_setting_read_default_view(const struct zmk_custom_setting *setting,
                                         struct zmk_custom_setting_value_view *value);
int zmk_custom_setting_read_by_key_view(const char *custom_subsystem_id, const char *key,
                                        struct zmk_custom_setting_value_view *value);
int zmk_custom_setting_read_array_by_key_view(const char *custom_subsystem_id, const char *key,
                                              uint32_t index,
                                              struct zmk_custom_setting_value_view *value);
int zmk_custom_setting_serialize_rpc_value_view(
    const struct zmk_custom_setting *setting,
    const struct zmk_custom_setting_value_view *internal_value,
    struct zmk_custom_setting_value_view *rpc_value);
int zmk_custom_setting_deserialize_rpc_value_view(
    const struct zmk_custom_setting *setting, const struct zmk_custom_setting_value_view *rpc_value,
    struct zmk_custom_setting_value_view *internal_value);
int zmk_custom_setting_write_view(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_setting_value_view *value,
                                  enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_write_by_key_view(const char *custom_subsystem_id, const char *key,
                                         const struct zmk_custom_setting_value_view *value,
                                         enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_write_array_by_key_view(const char *custom_subsystem_id, const char *key,
                                               uint32_t index,
                                               const struct zmk_custom_setting_value_view *value,
                                               enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_write_array_element_view(const struct zmk_custom_setting *setting,
                                                const struct zmk_custom_setting_value_view *value,
                                                uint32_t array_size,
                                                enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_array_push_back_view(const struct zmk_custom_setting *setting,
                                            const struct zmk_custom_setting_value_view *value,
                                            enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_array_pop_back_view(const struct zmk_custom_setting *setting,
                                           struct zmk_custom_setting_value_view *value,
                                           enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_array_insert_at_view(
    const struct zmk_custom_setting *array_setting_or_element, uint32_t index,
    const struct zmk_custom_setting_value_view *value, enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_array_remove_at_view(
    const struct zmk_custom_setting *array_setting_or_element, uint32_t index,
    struct zmk_custom_setting_value_view *out_value, enum zmk_custom_setting_write_mode mode);
int zmk_custom_setting_validate_view(const struct zmk_custom_setting *setting,
                                     const struct zmk_custom_setting_value_view *value);
/* Visits synchronously under the settings lock. Neither the view nor its
 * payload may escape. Do not call settings APIs: a nested read may overwrite
 * shared scratch, and a write may move the pool bytes backing this view.
 * Unlike ref_visit, this callback borrows the value itself. */
int zmk_custom_setting_with_view(const struct zmk_custom_setting *setting,
                                 zmk_custom_setting_view_visitor_t visitor, void *user_data);
int zmk_custom_setting_keyspace_create_view(struct zmk_custom_setting_keyspace *keyspace,
                                            const char *key,
                                            const struct zmk_custom_setting_value_view *value,
                                            enum zmk_custom_setting_write_mode mode,
                                            const struct zmk_custom_setting **out_setting);

/* Visits synchronously under the settings lock. The view and its payload must
 * not escape; copy needed data before returning. Do not call settings APIs
 * from a visitor. The default variant returns -ENOENT if no default exists. */
int zmk_custom_setting_with_default_view(const struct zmk_custom_setting *setting,
                                         zmk_custom_setting_view_visitor_t visitor,
                                         void *user_data);

/* Runtime-length blob constructor: rejects overflow before narrowing size_t. */
int zmk_custom_setting_view_blob(struct zmk_custom_setting_value_view *value,
                                 enum zmk_custom_setting_value_type type, const void *data,
                                 size_t size);

#ifndef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
/* The old spelling uses the identical compact contract when compatibility is
 * off. No wrapper functions or duplicate value types are linked. */
#define zmk_custom_setting_read zmk_custom_setting_read_view
#define zmk_custom_setting_read_default zmk_custom_setting_read_default_view
#define zmk_custom_setting_read_by_key zmk_custom_setting_read_by_key_view
#define zmk_custom_setting_read_array_by_key zmk_custom_setting_read_array_by_key_view
#define zmk_custom_setting_serialize_rpc_value zmk_custom_setting_serialize_rpc_value_view
#define zmk_custom_setting_deserialize_rpc_value zmk_custom_setting_deserialize_rpc_value_view
#define zmk_custom_setting_write zmk_custom_setting_write_view
#define zmk_custom_setting_write_by_key zmk_custom_setting_write_by_key_view
#define zmk_custom_setting_write_array_by_key zmk_custom_setting_write_array_by_key_view
#define zmk_custom_setting_write_array_element zmk_custom_setting_write_array_element_view
#define zmk_custom_setting_array_push_back zmk_custom_setting_array_push_back_view
#define zmk_custom_setting_array_pop_back zmk_custom_setting_array_pop_back_view
#define zmk_custom_setting_array_insert_at zmk_custom_setting_array_insert_at_view
#define zmk_custom_setting_array_remove_at zmk_custom_setting_array_remove_at_view
#define zmk_custom_setting_validate zmk_custom_setting_validate_view
#define zmk_custom_setting_with_value zmk_custom_setting_with_view
#define zmk_custom_setting_keyspace_create zmk_custom_setting_keyspace_create_view
#endif
