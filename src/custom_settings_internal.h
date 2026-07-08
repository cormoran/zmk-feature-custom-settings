/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Private, shared-within-module contract between src/custom_settings.c
 * (core lifecycle: always compiled with ZMK_CUSTOM_SETTINGS) and the
 * optional feature translation units split out of it: src/custom_settings_pool.c
 * (CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES) and src/custom_settings_array.c
 * (CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY).
 *
 * NOT a public header - lives under src/, not include/. Only files inside
 * this module may include it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/kernel.h>

#include <cormoran/zmk/custom_settings.h>

/* Shared settings-subsystem naming, used by both custom_settings.c
 * (setting_storage_name) and custom_settings_array.c (array_size_storage_name/
 * split_array_size_key) to agree on the on-flash key layout. */
#define SETTINGS_SUBTREE "custom_settings"
#define ARRAY_SIZE_STORAGE_KEY "_size"

/* The single lock serializing all custom-settings state access. Defined
 * (non-static) in custom_settings.c so every feature translation unit can
 * take it too. */
extern struct k_mutex custom_settings_lock;

/*
 * ---------------------------------------------------------------------
 * POOL entry points (defined in custom_settings_pool.c when
 * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES is enabled). Declared here
 * unconditionally so core call sites can be IS_ENABLED-guarded without
 * custom_settings_pool.c needing to be compiled.
 * ---------------------------------------------------------------------
 */

/* Ensure a pooled setting (setting->blob.pool != NULL) has a region of at
 * least `needed` bytes, compacting other pool members if necessary. See the
 * implementation comment in custom_settings_pool.c for the full contract. */
int pool_ensure_region(const struct zmk_custom_setting *setting, size_t needed);

/* Release `setting`'s pool region (if any) and unlink it from its pool's
 * member list. Caller holds custom_settings_lock. */
void pool_release_locked(const struct zmk_custom_setting *setting);

/* Apply a large (> carrier) raw BYTES/STRING payload to a blob setting.
 * Caller holds custom_settings_lock. */
int write_large_locked(const struct zmk_custom_setting *setting, const void *data, size_t size,
                       enum zmk_custom_setting_write_mode mode);

/*
 * ---------------------------------------------------------------------
 * ARRAY entry points (defined in custom_settings_array.c when
 * CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY is enabled). Declared here
 * unconditionally so core call sites can be guarded by
 * zmk_custom_setting_is_array() (which itself folds to a compile-time
 * constant when the feature is off - see the inline definition in
 * include/cormoran/zmk/custom_settings.h) without custom_settings_array.c
 * needing to be compiled.
 * ---------------------------------------------------------------------
 */

/* Acquire the shared "index view" for one (array descriptor, index) pair -
 * see the struct zmk_custom_setting_array_view_slot doc in
 * custom_settings_array.c. Caller holds custom_settings_lock. */
struct zmk_custom_setting *array_view_acquire(const struct zmk_custom_setting *array_descriptor,
                                              uint32_t index);

/* Per-array-element dirty/has-persistent flag storage (array_state->dirty[]/
 * has_persistent[]), the array counterpart of the scalar
 * ZMK_CUSTOM_SETTING_STATE_DIRTY/HAS_PERSISTENT flags. Caller holds
 * custom_settings_lock. */
bool *array_dirty_slot(const struct zmk_custom_setting *setting);
bool *array_has_persistent_slot(const struct zmk_custom_setting *setting);

/* Set an array's active (in-memory) / persisted length, clearing any
 * temporary override past the new size. Caller holds custom_settings_lock. */
void set_array_memory_size_locked(const struct zmk_custom_setting *array_element,
                                  uint32_t array_size);
void set_array_persistent_size_locked(const struct zmk_custom_setting *array_element,
                                      uint32_t array_size);

/* Settings-subtree name of an array's "_size" marker record. */
int array_size_storage_name(const struct zmk_custom_setting *setting, char *name, size_t name_size);

/* Persist an array setting: every active element plus the "_size" marker,
 * then delete any now-inactive elements' records. Caller holds
 * custom_settings_lock. */
int save_array_locked(const struct zmk_custom_setting *array_descriptor);

/* Discard/reset one array element view's in-memory value, the array
 * counterparts of the scalar zmk_custom_setting_discard/reset bodies.
 * Caller holds custom_settings_lock. */
int discard_array_element_locked(struct zmk_custom_setting *view);
int reset_array_element_locked(struct zmk_custom_setting *view);

/* Apply a loaded "_size" record's raw payload to an array's persistent/
 * memory size. Caller holds custom_settings_lock. */
int array_size_from_storage(const struct zmk_custom_setting *array_element, const void *data,
                            size_t len);

/* Recognize a stored settings name as an array's "_size" marker
 * (split_array_size_key) or one array element record (split_array_element_key),
 * extracting the array's public key (and, for the latter, the element
 * index). Used by custom_settings_handle_set (the settings-load callback) to
 * route a loaded record to the right array element/marker. */
bool split_array_size_key(const char *name, char *array_key, size_t array_key_size);
bool split_array_element_key(const char *name, char *array_key, size_t array_key_size,
                             uint32_t *index);

/*
 * ---------------------------------------------------------------------
 * CORE internals used by custom_settings_pool.c / custom_settings_array.c
 * (and any future feature translation unit). Defined (non-static) in
 * custom_settings.c.
 * ---------------------------------------------------------------------
 */

/* True for a non-array BYTES/STRING setting, i.e. one whose value lives in
 * the blob store (fixed buffer or pool region) rather than a scalar union
 * member. */
bool setting_uses_blob_store(const struct zmk_custom_setting *setting);

/* True while `setting` has an active TEMPORARY-mode override. */
bool setting_temporary_active(const struct zmk_custom_setting *setting);

/* Maximum payload size `setting` may ever hold (its own blob.max_size for a
 * non-array BYTES/STRING setting, else the global carrier size). */
size_t setting_capacity(const struct zmk_custom_setting *setting);

/* Discard any active TEMPORARY-mode override for `setting`. Caller holds
 * custom_settings_lock. */
void clear_temporary_locked(const struct zmk_custom_setting *setting);

/* Mark `setting`'s in-memory value as (not) differing from its persisted
 * value. */
void set_setting_dirty(const struct zmk_custom_setting *setting, bool dirty);

/* Persist `setting`'s current in-memory value to flash. Caller holds
 * custom_settings_lock. */
int save_setting_locked(const struct zmk_custom_setting *setting);

/* Length of the embedded "user_key\0" prefix stored ahead of a keyspace
 * slot's payload in its blob, or 0 for a non-keyspace setting. Caller holds
 * custom_settings_lock. */
size_t keyspace_blob_key_len_locked(const struct zmk_custom_setting *setting);

/* Store `size` raw payload bytes into a blob setting's store (fixed buffer
 * or, for a pooled setting, a pool region obtained via pool_ensure_region()).
 * Caller must have validated size <= setting_capacity(setting). */
int blob_store_set_raw(const struct zmk_custom_setting *setting, const void *data, size_t size);

/* Bounded strnlen, used throughout the module wherever a raw string field
 * (never guaranteed NUL-terminated within its buffer) needs a safe length. */
size_t bounded_strlen(const char *str, size_t max_len);

/* Copy `src` into `dest`, re-deriving STRING's `size` from its NUL terminator
 * (see the definition for why: `size` is not always trustworthy on the
 * source side). */
void copy_value(struct zmk_custom_setting_value *dest, const struct zmk_custom_setting_value *src);

/* Split `value` into a (data pointer, length) pair suitable for
 * settings_save_one/memcpy, encoding a BEHAVIOR value into a shared scratch
 * buffer first if needed. Caller holds custom_settings_lock (the BEHAVIOR
 * scratch buffer is not otherwise synchronized). */
int value_to_storage(const struct zmk_custom_setting_value *value, const void **data, size_t *len);

/* Settings-subtree name of `setting`'s own record (a scalar/blob setting's
 * key, or - for an array element view - "<array_key>/<index>"). */
int setting_storage_name(const struct zmk_custom_setting *setting, char *name, size_t name_size);

/* Return the effective value (temporary override if active, else the memory
 * value) as a fixed carrier, or NULL when the value is a blob payload too
 * large for the carrier. Caller holds custom_settings_lock; the returned
 * pointer is only valid until the lock is released (shared scratch storage,
 * like every other *_scratch_value in custom_settings.c). */
const struct zmk_custom_setting_value *effective_value(const struct zmk_custom_setting *setting);

/* Apply a write in the selected mode (TEMPORARY/MEMORY/PERSIST) to an
 * already-resolved, in-range setting or array element view. Caller holds
 * custom_settings_lock. */
int write_value_locked(const struct zmk_custom_setting *setting,
                       const struct zmk_custom_setting_value *value,
                       enum zmk_custom_setting_write_mode mode);

/* Raise a ZMK_CUSTOM_SETTING_CHANGED event for `setting`, sourced locally. */
void raise_setting_changed(const struct zmk_custom_setting *setting,
                           enum zmk_custom_setting_changed_kind kind);
