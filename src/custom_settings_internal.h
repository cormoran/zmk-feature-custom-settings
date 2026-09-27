/* SPDX-License-Identifier: MIT */
/* Private contracts. Unless stated otherwise, the caller owns
 * custom_settings_lock; these helpers never retain borrowed input pointers. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/kernel.h>

#include <cormoran/zmk/custom_settings.h>

#define SETTINGS_SUBTREE "custom_settings"
#define ARRAY_SIZE_STORAGE_KEY "_size"

/* One lock covers values, pool nodes, array flags and temporary overlays.
 * Acquire outer RPC/session locks before this lock. */
extern struct k_mutex custom_settings_lock;

/* Descriptor adapters around the generic allocator. */
void pool_release_locked(const struct zmk_custom_setting *setting);

int write_large_locked(const struct zmk_custom_setting *setting, const void *data, size_t size,
                       enum zmk_custom_setting_write_mode mode);

/* Array lifecycle helpers; storage/bitsets live in array_storage.h. */
#include "custom_settings_views.h"
void apply_scalar_default_locked(const struct zmk_custom_setting *setting);
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
int compat_blob_store_set_raw(const struct zmk_custom_setting *setting, const void *data,
                              size_t size);
bool compat_array_default(const struct zmk_custom_setting *array, uint32_t index);
#endif

void set_array_memory_size_locked(const struct zmk_custom_setting *array_element,
                                  uint32_t array_size);
void set_array_persistent_size_locked(const struct zmk_custom_setting *array_element,
                                      uint32_t array_size);

int array_size_storage_name(const struct zmk_custom_setting *setting, char *name, size_t name_size);

int save_array_locked(const struct zmk_custom_setting *array_descriptor);

int discard_array_element_locked(struct zmk_custom_setting *view);
int reset_array_element_locked(struct zmk_custom_setting *view);

int array_size_from_storage(const struct zmk_custom_setting *array_element, const void *data,
                            size_t len);

bool split_array_size_key(const char *name, char *array_key, size_t array_key_size);
bool split_array_element_key(const char *name, char *array_key, size_t array_key_size,
                             uint32_t *index);

/* Keyspace helpers preserve the [user_key NUL][payload] storage format. */
int keyspace_read_payload(const struct zmk_custom_setting *setting,
                          struct zmk_custom_setting_value_view *out_value);

int keyspace_read_into(const struct zmk_custom_setting *setting, void *buf, size_t capacity,
                       size_t *out_size, enum zmk_custom_setting_value_type *out_type);

int keyspace_validate_payload(const struct zmk_custom_setting_keyspace *keyspace,
                              const struct zmk_custom_setting_value_view *value);

int keyspace_write_blob(const struct zmk_custom_setting *setting, const char *key,
                        const struct zmk_custom_setting_value_view *value,
                        enum zmk_custom_setting_write_mode mode);

int keyspace_write_raw_payload(const struct zmk_custom_setting *setting, const void *data,
                               size_t size, enum zmk_custom_setting_write_mode mode);

void keyspace_release_slot_locked(struct zmk_custom_setting_keyspace *keyspace, uint32_t index);

void keyspace_release_slot_for_setting_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              const struct zmk_custom_setting *setting);

struct zmk_custom_setting *keyspace_bind_slot_locked(struct zmk_custom_setting_keyspace *keyspace,
                                                     uint32_t index);

bool keyspace_parse_ordinal_name(const struct zmk_custom_setting_keyspace *keyspace,
                                 const char *name, uint32_t *out_index);

const char *keyspace_public_key_locked(const struct zmk_custom_setting *setting);

#define ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE                                             \
    (CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN + CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE)

int convert_rpc_bytes_value(const struct zmk_custom_setting *setting,
                            const struct zmk_custom_setting_value_view *src,
                            struct zmk_custom_setting_value_view *dest,
                            zmk_custom_setting_rpc_bytes_converter_t converter);

bool setting_uses_blob_store(const struct zmk_custom_setting *setting);

bool setting_temporary_active(const struct zmk_custom_setting *setting);

size_t setting_capacity(const struct zmk_custom_setting *setting);

void clear_temporary_locked(const struct zmk_custom_setting *setting);

void set_setting_dirty(const struct zmk_custom_setting *setting, bool dirty);

int save_setting_locked(const struct zmk_custom_setting *setting);

size_t keyspace_blob_key_len_locked(const struct zmk_custom_setting *setting);

int blob_store_set_raw(const struct zmk_custom_setting *setting, const void *data, size_t size);

size_t bounded_strlen(const char *str, size_t max_len);

/* dest.size is capacity on entry, payload length on success. Pointer payloads
 * are copied into caller storage; reset the output buffer before each reuse. */
int copy_value(struct zmk_custom_setting_value_view *dest,
               const struct zmk_custom_setting_value_view *src);

int value_to_storage(const struct zmk_custom_setting_value_view *value, const void **data,
                     size_t *len);

int setting_storage_name(const struct zmk_custom_setting *setting, char *name, size_t name_size);

/* Returns shared scratch or borrowed storage. Consume under the lock before
 * another settings operation; holding the recursive mutex alone is not enough. */
const struct zmk_custom_setting_value_view *
effective_value(const struct zmk_custom_setting *setting);

int write_value_locked(const struct zmk_custom_setting *setting,
                       const struct zmk_custom_setting_value_view *value,
                       enum zmk_custom_setting_write_mode mode);

void raise_setting_changed(const struct zmk_custom_setting *setting,
                           enum zmk_custom_setting_changed_kind kind);

int value_from_raw(struct zmk_custom_setting_value_view *dest,
                   enum zmk_custom_setting_value_type type, const void *data, size_t size,
                   struct zmk_custom_setting_behavior_value *behavior);

int write_bytes_raw(const struct zmk_custom_setting *setting, const void *data, size_t size,
                    enum zmk_custom_setting_write_mode mode);

void init_setting_state_locked(const struct zmk_custom_setting *setting);

struct read_into_context {
    void *buf;
    size_t capacity;
    size_t out_size;
    enum zmk_custom_setting_value_type out_type;
    int ret;
};
void read_into_visitor(const struct zmk_custom_setting_value_view *value, void *user_data);

/* Synchronous workspace, borrowed only while holding the settings lock.
 * Chunk input has a longer lifetime and owns a separate buffer. */
#define CUSTOM_SETTINGS_EDIT_SIZE                                                                  \
    (CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN +                                                      \
     MAX(CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,                                          \
         CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) +                                              \
     1)
extern uint8_t custom_settings_edit_bytes[CUSTOM_SETTINGS_EDIT_SIZE];
int blob_write_locked(struct zmk_custom_setting_large_pool *pool,
                      struct zmk_custom_setting_blob *blob, const void *data, size_t size,
                      bool terminate);

void clear_array_temporary_locked(const struct zmk_custom_setting_array_state *array,
                                  uint32_t first);

int persist_raw_candidate_locked(const struct zmk_custom_setting *setting, const void *data,
                                 size_t size);

/* Static metadata is translated only while legacy ownership is enabled. */
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
struct zmk_custom_setting_value_view value_borrow(const struct zmk_custom_setting_value *value);
struct zmk_custom_setting_value_view value_output(struct zmk_custom_setting_value *value);
int value_finish(struct zmk_custom_setting_value *value,
                 const struct zmk_custom_setting_value_view *view);

int value_check_input(const struct zmk_custom_setting_value *value);

int value_visit(const struct zmk_custom_setting_value_view *view,
                zmk_custom_setting_value_visitor_t visitor, void *user_data);

#else
static inline struct zmk_custom_setting_value_view
value_borrow(const struct zmk_custom_setting_value_view *value) {
    return *value;
}
#endif

int keyspace_payload_view_locked(const struct zmk_custom_setting *setting,
                                 struct zmk_custom_setting_value_view *out_value,
                                 struct zmk_custom_setting_behavior_value *behavior);
