/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Private, shared-within-module contract between src/custom_settings.c
 * (core lifecycle: always compiled with ZMK_CUSTOM_SETTINGS) and the
 * optional feature translation units split out of it (starting with
 * src/custom_settings_pool.c, gated by
 * CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES).
 *
 * NOT a public header - lives under src/, not include/. Only files inside
 * this module may include it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/kernel.h>

#include <cormoran/zmk/custom_settings.h>

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
 * CORE internals used by custom_settings_pool.c (and any future feature
 * translation unit). Defined (non-static) in custom_settings.c.
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
