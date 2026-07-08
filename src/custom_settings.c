/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <cormoran/zmk/custom_settings.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_custom_setting_changed);

#define SETTINGS_SUBTREE "custom_settings"
#define ARRAY_SIZE_STORAGE_KEY "_size"

static K_MUTEX_DEFINE(settings_lock);

/* Forward declaration: defined alongside the rest of the scalar setting
 * lifecycle further down, but also needed by the keyspace slot bind path
 * (keyspace_bind_slot_locked, defined earlier in this file) to reset a
 * freshly (re)bound slot's mutable state (has_persistent_value/dirty/
 * temp_slot/initialized) the same way a compile-time setting is
 * initialized at boot. */
static void init_setting_state_locked(const struct zmk_custom_setting *setting);

/*
 * Forward declarations for the keyspace slot presentation helpers
 * (simplification P3, full implementation further down alongside
 * zmk_custom_setting_keyspace_create/delete/find) - needed because the
 * generic read/write/find/public_key functions defined earlier in this file
 * consult them to treat a live keyspace slot's opaque `[user_key\0][payload]`
 * blob correctly (see the keyspace design comment in the header and further
 * down in this file).
 */
static size_t keyspace_blob_key_len_locked(const struct zmk_custom_setting *setting);
static int keyspace_read_payload(const struct zmk_custom_setting *setting,
                                 struct zmk_custom_setting_value *out_value);
static int keyspace_read_into(const struct zmk_custom_setting *setting, void *buf, size_t capacity,
                              size_t *out_size, enum zmk_custom_setting_value_type *out_type);
static int keyspace_validate_payload(const struct zmk_custom_setting_keyspace *keyspace,
                                     const struct zmk_custom_setting_value *value);
static int keyspace_write_blob(const struct zmk_custom_setting *setting, const char *key,
                               const struct zmk_custom_setting_value *value,
                               enum zmk_custom_setting_write_mode mode);
static int keyspace_write_raw_payload(const struct zmk_custom_setting *setting, const void *data,
                                      size_t size, enum zmk_custom_setting_write_mode mode);

/* Forward declaration: array_view_acquire() (defined below) recycles pooled
 * views and must release any temporary override the evicted view still owns,
 * but clear_temporary_locked() is defined further down alongside the rest of
 * the temp-slot helpers. */
static void clear_temporary_locked(const struct zmk_custom_setting *setting);

static size_t bounded_strlen(const char *str, size_t max_len) {
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        len++;
    }

    return len;
}

static bool value_equals(const struct zmk_custom_setting_value *a,
                         const struct zmk_custom_setting_value *b) {
    if (a->type != b->type) {
        return false;
    }

    switch (a->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        return a->size == b->size && memcmp(a->bytes_value, b->bytes_value, a->size) == 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        return a->int32_value == b->int32_value;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        return a->bool_value == b->bool_value;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        return strncmp(a->string_value, b->string_value,
                       CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) == 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        return a->behavior_value.behavior_id == b->behavior_value.behavior_id &&
               a->behavior_value.param1 == b->behavior_value.param1 &&
               a->behavior_value.param2 == b->behavior_value.param2;
    default:
        return false;
    }
}

static void copy_value(struct zmk_custom_setting_value *dest,
                       const struct zmk_custom_setting_value *src) {
    *dest = *src;
    if (dest->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        dest->string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE] = '\0';
        dest->size = bounded_strlen(dest->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    }
}

/*
 * Simplification P4: state flag helpers. All mutable per-setting flags live
 * as bits in setting->state->flags (see struct zmk_custom_setting_state in
 * the header); the descriptor itself is const. Caller holds settings_lock
 * for any use that must be coherent with other state fields.
 */
static bool state_flag(const struct zmk_custom_setting *setting, uint8_t flag) {
    return (setting->state->flags & flag) != 0;
}

static void state_flag_set(const struct zmk_custom_setting *setting, uint8_t flag, bool value) {
    if (value) {
        setting->state->flags |= flag;
    } else {
        setting->state->flags &= ~flag;
    }
}

static bool setting_temporary_active(const struct zmk_custom_setting *setting) {
    return state_flag(setting, ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE);
}

/*
 * Per-setting BYTES/STRING blob store (issue #16, reshaped by
 * simplification P4).
 *
 * Every non-array BYTES/STRING setting keeps its payload behind
 * state->blob.data: either the exact-size static buffer the plain
 * ZMK_CUSTOM_SETTING_DEFINE macro emits (descriptor blob.pool == NULL; the
 * pointer never changes), or a region carved on demand from a shared pool
 * (ZMK_CUSTOM_SETTING_DEFINE_POOLED / _SIZED's private pool / a keyspace's
 * slot pool; blob.data is NULL until the first non-empty write - see
 * pool_ensure_region() below). The pre-P4 embedded `memory_value` carrier is
 * gone, so there is no small/large fork: these helpers are the single
 * read/write path for blob values of any size, and `blob.pool == NULL` is
 * the only fixed-buffer vs pool-member distinction.
 */
static size_t setting_capacity(const struct zmk_custom_setting *setting) {
    if (!zmk_custom_setting_is_array(setting) &&
        (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
         setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING)) {
        return setting->blob.max_size;
    }
    return CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE;
}

static bool setting_uses_blob_store(const struct zmk_custom_setting *setting) {
    return !zmk_custom_setting_is_array(setting) &&
           (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
            setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING);
}

/* Bytes a pool member currently occupies: its payload plus the trailing NUL
 * for STRING (blob_store_set_raw always writes that NUL inside the region,
 * so it must count toward the region's extent). 0 for a member with no
 * region (blob.data == NULL). */
static size_t pool_member_extent(const struct zmk_custom_setting *setting) {
    if (setting->state->blob.data == NULL) {
        return 0;
    }
    return setting->state->blob.size +
           (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING ? 1 : 0);
}

/* Link `setting` into `pool`'s intrusive member list. Caller holds
 * settings_lock and must only call this the moment `setting->state->blob.data`
 * transitions from NULL to non-NULL (i.e. `setting` is not already a
 * member). */
static void pool_link_locked(struct zmk_custom_setting_large_pool *pool,
                             const struct zmk_custom_setting *setting) {
    setting->state->_pool_next = pool->members;
    pool->members = setting;
}

/* Unlink `setting` from `pool`'s intrusive member list. Caller holds
 * settings_lock. A no-op if `setting` is not currently a member (e.g. it
 * never allocated a region). */
static void pool_unlink_locked(struct zmk_custom_setting_large_pool *pool,
                               const struct zmk_custom_setting *setting) {
    if (pool->members == setting) {
        pool->members = setting->state->_pool_next;
        setting->state->_pool_next = NULL;
        return;
    }

    const struct zmk_custom_setting *prev = pool->members;
    while (prev && prev->state->_pool_next != setting) {
        prev = prev->state->_pool_next;
    }
    if (prev) {
        prev->state->_pool_next = setting->state->_pool_next;
        setting->state->_pool_next = NULL;
    }
}

/*
 * Release `setting`'s pool region (if any) and unlink it from its pool's
 * member list. Caller holds settings_lock. Must run before a keyspace
 * slot's storage is reused by a future entry (keyspace_release_slot_locked,
 * called from zmk_custom_setting_keyspace_delete) - otherwise the pool's
 * member list would keep a dangling pointer at storage that no longer
 * represents this setting (see the intrusive-list note on struct
 * zmk_custom_setting_large_pool). A no-op for a non-pooled setting or one
 * with no allocated region.
 */
static void pool_release_locked(const struct zmk_custom_setting *setting) {
    if (setting->blob.pool == NULL || setting->state->blob.data == NULL) {
        return;
    }
    pool_unlink_locked(setting->blob.pool, setting);
    setting->state->blob.data = NULL;
    setting->state->blob.size = 0;
}

/*
 * Ensure a pooled setting (setting->blob.pool != NULL) has a region of at
 * least `needed` bytes (payload plus the STRING NUL, if any - the caller
 * computes this the same way pool_member_extent does). Caller holds
 * settings_lock. Returns 0 on success or -ENOSPC if the pool cannot fit
 * `needed` bytes for this setting even after compacting every other member;
 * on -ENOSPC nothing is modified - not `setting`, not any other pool
 * member - so the write that requested this region can fail cleanly with the
 * setting's previous value untouched.
 *
 * Deliberately simple: no free lists, no per-region headers, no cached
 * bookkeeping. A region's extent is always derived on the spot from its
 * owning setting's current blob.size, so "compacting" just means walking
 * every OTHER member of this pool (via the pool's own intrusive member list,
 * not the global setting registry - see struct zmk_custom_setting_large_pool),
 * sliding each down (in ascending address order, via memmove, which
 * tolerates the overlapping src/dest this can produce) to eliminate any gap
 * starting at pool->data, and finally handing this setting whatever space is
 * left right after the last one. That happens on every growth, not only when
 * strictly necessary, but pools are expected to hold a handful of members, so
 * this stays cheap and never needs invalidating.
 *
 * Selecting members in ascending address order is done without an
 * intermediate array (repeatedly scanning for the lowest not-yet-processed
 * address) so this has no fixed cap on pool membership. Every member walked
 * here (other than `setting` itself, which may or may not be one yet) is by
 * construction one with blob.data != NULL, since that is the pool's
 * membership invariant - see pool_link_locked/pool_unlink_locked.
 */
static int pool_ensure_region(const struct zmk_custom_setting *setting, size_t needed) {
    struct zmk_custom_setting_large_pool *pool = setting->blob.pool;

    if (needed == 0) {
        if (setting->state->blob.data != NULL) {
            pool_unlink_locked(pool, setting);
            setting->state->blob.data = NULL;
        }
        return 0;
    }

    if (setting->state->blob.data != NULL && needed <= pool_member_extent(setting)) {
        /* Shrinking (or an unchanged size) keeps writing into the same
         * region - no need to move anything. */
        return 0;
    }

    /* Feasibility check before moving anything: every other member of this
     * pool keeps its current extent, plus the room this write needs. */
    size_t other_total = 0;
    for (const struct zmk_custom_setting *other = pool->members; other;
         other = other->state->_pool_next) {
        if (other == setting) {
            continue;
        }
        other_total += pool_member_extent(other);
    }
    if (other_total + needed > pool->size) {
        return -ENOSPC;
    }

    uint8_t *cursor = pool->data;
    bool have_last_addr = false;
    const uint8_t *last_addr = NULL;
    for (;;) {
        const struct zmk_custom_setting *next = NULL;
        for (const struct zmk_custom_setting *other = pool->members; other;
             other = other->state->_pool_next) {
            if (other == setting) {
                continue;
            }
            if (have_last_addr && other->state->blob.data <= last_addr) {
                /* Already compacted in an earlier iteration. */
                continue;
            }
            if (!next || other->state->blob.data < next->state->blob.data) {
                next = other;
            }
        }
        if (!next) {
            break;
        }

        have_last_addr = true;
        last_addr = next->state->blob.data;

        size_t extent = pool_member_extent(next);
        if (next->state->blob.data != cursor) {
            memmove(cursor, next->state->blob.data, extent);
            next->state->blob.data = cursor;
        }
        cursor += extent;
    }

    bool was_member = setting->state->blob.data != NULL;
    setting->state->blob.data = cursor;
    if (!was_member) {
        pool_link_locked(pool, setting);
    }
    return 0;
}

/* Store `size` raw payload bytes into a blob setting's store. Caller must
 * have validated size <= setting_capacity(setting). For a pooled setting,
 * first (re)points blob.data at a big-enough region via pool_ensure_region(),
 * possibly relocating other members of the same pool; on -ENOSPC nothing is
 * modified. For a fixed-store setting (descriptor blob.pool == NULL) the
 * buffer never moves and, being sized capacity + 1 by the registration
 * macro, always has room for the STRING NUL. */
static int blob_store_set_raw(const struct zmk_custom_setting *setting, const void *data,
                              size_t size) {
    if (setting->blob.pool != NULL) {
        size_t needed =
            size + (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING ? 1 : 0);
        int ret = pool_ensure_region(setting, needed);
        if (ret < 0) {
            return ret;
        }
    }

    if (size > 0) {
        memcpy(setting->state->blob.data, data, size);
    }
    if (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING &&
        setting->state->blob.data != NULL) {
        setting->state->blob.data[size] = '\0';
    }
    setting->state->blob.size = size;
    return 0;
}

/* Copy a carrier value (BYTES/STRING, <= carrier size) into a blob setting's
 * store - the normal zmk_custom_setting_write / default-application path.
 * Returns -ENOSPC for a pooled setting whose backing pool has no room (see
 * blob_store_set_raw); the setting's previous value is left untouched in
 * that case. */
static int blob_store_set_value(const struct zmk_custom_setting *setting,
                                const struct zmk_custom_setting_value *value) {
    if (value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        size_t len = bounded_strlen(value->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        len = MIN(len, setting_capacity(setting));
        return blob_store_set_raw(setting, value->string_value, len);
    } else {
        size_t len = MIN(value->size, setting_capacity(setting));
        return blob_store_set_raw(setting, value->bytes_value, len);
    }
}

/* The default a setting currently falls back to: the runtime override
 * installed by zmk_custom_setting_set_default() if any, else the
 * registration-time (flash) default. */
static const struct zmk_custom_setting_value *
setting_default_value(const struct zmk_custom_setting *setting) {
    return setting->state->default_override ? setting->state->default_override
                                            : setting->default_value;
}

/* Store a validated carrier value as a non-array setting's in-memory value:
 * blob settings route to their store (pool region or fixed buffer), scalars
 * inline into the right-sized state union. Caller holds settings_lock.
 * Returns -ENOSPC only for a pooled blob setting out of pool room. */
static int store_scalar_value_locked(const struct zmk_custom_setting *setting,
                                     const struct zmk_custom_setting_value *value) {
    if (setting_uses_blob_store(setting)) {
        return blob_store_set_value(setting, value);
    }

    struct zmk_custom_setting_state *state = setting->state;
    switch (setting->value_type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        state->int32_value = value->int32_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        state->bool_value = value->bool_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        state->behavior = value->behavior_value;
        return 0;
    default:
        return -EINVAL;
    }
}

/* Apply a scalar setting's default (override or compile-time) to its memory
 * storage. Caller holds settings_lock. An empty (size 0) default never
 * allocates a pool region (blob_store_set_raw's needed == 0 case). A
 * pooled setting whose non-empty default cannot fit its pool at init time
 * logs and leaves the setting without a region rather than failing boot. */
static void apply_scalar_default_locked(const struct zmk_custom_setting *setting) {
    if (setting->_keyspace) {
        /* A keyspace slot has no compile-time default payload: its blob
         * always carries a user key that only exists once the entry is
         * created/bound, so there is no shared static value that could
         * represent "this not-yet-existing entry's default". Leave the
         * blob untouched here - a freshly bound slot starts empty
         * (blob.data == NULL) and is populated moments later either by
         * the settings-load value-apply step or by keyspace_write_blob()
         * (create). This also means discard()/reset() on a keyspace entry
         * with no persisted record are no-ops rather than reverting to a
         * default unrelated to the entry's own key - see the keyspace
         * design comment in the header and the Phase 3 design doc
         * addendum. */
        return;
    }
    int ret = store_scalar_value_locked(setting, setting_default_value(setting));
    if (ret < 0) {
        LOG_ERR("Custom settings pool has no room for %s/%s's default value (%d)",
                setting->custom_subsystem_id, setting->key, ret);
    }
}

size_t zmk_custom_setting_large_pool_used(const struct zmk_custom_setting_large_pool *pool) {
    if (!pool) {
        return 0;
    }

    size_t used = 0;
    k_mutex_lock(&settings_lock, K_FOREVER);
    for (const struct zmk_custom_setting *member = pool->members; member;
         member = member->state->_pool_next) {
        used += pool_member_extent(member);
    }
    k_mutex_unlock(&settings_lock);
    return used;
}

/*
 * Temporary overrides (ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY) are rare and
 * short-lived, so instead of a full struct zmk_custom_setting_value slot
 * embedded in every registered setting, a small shared pool holds the few
 * that are active at once. Settings whose value is larger than one slot
 * simply cannot use temporary mode (-EMSGSIZE).
 */
struct zmk_custom_settings_temp_slot {
    bool in_use;
    const struct zmk_custom_setting *owner;
    enum zmk_custom_setting_value_type type;
    size_t size;
    uint8_t data[CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE];
};

static struct zmk_custom_settings_temp_slot temp_slots[CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS];
/* Scratch space effective_value() reconstructs a temporary override into.
 * Safe as a single shared instance: all access happens while settings_lock
 * is held. */
static struct zmk_custom_setting_value temp_scratch_value;
/* Scratch space effective_value() materializes a large-store setting's value
 * into when it still fits the fixed carrier. Same single-shared-instance
 * safety as temp_scratch_value (settings_lock held). */
static struct zmk_custom_setting_value effective_scratch_value;

/*
 * Array element "index views": zmk_custom_setting_find_array_element hands
 * out a struct zmk_custom_setting * for one (array descriptor, index) pair
 * without registering a real STRUCT_SECTION_ITERABLE entry per element (see
 * struct zmk_custom_setting_array_state in the header). Views are pulled
 * from this small direct-mapped pool and reused for the same (array, index)
 * pair across calls, so a pointer returned earlier keeps working correctly
 * (including any temp_slot it owns) even if other lookups happened in
 * between - this matters for callers like test_temporary_override_pool that
 * hold onto array element pointers across several calls.
 *
 * Pool size is bounded by CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE;
 * it only needs to cover how many distinct array elements are referenced at
 * once (RPC handling, list enumeration, tests), not array length itself.
 */
struct zmk_custom_setting_array_view_slot {
    bool in_use;
    /* Simplification P4: a view is a runtime-built RAM instance of the
     * (normally const/flash) struct zmk_custom_setting, so it embeds its
     * own state block; the only view state that actually matters is the
     * TEMPORARY_ACTIVE flag + temp_slot (element values/dirty/has_persistent
     * live in the shared array_state). */
    struct zmk_custom_setting_state state;
    struct zmk_custom_setting view;
};

static struct zmk_custom_setting_array_view_slot
    array_view_pool[CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE];

static struct zmk_custom_setting *
array_view_acquire(const struct zmk_custom_setting *array_descriptor, uint32_t index) {
    struct zmk_custom_setting_array_view_slot *free_slot = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(array_view_pool); i++) {
        struct zmk_custom_setting_array_view_slot *slot = &array_view_pool[i];
        if (slot->in_use && slot->view.array_state == array_descriptor->array_state &&
            slot->view.array_index == index) {
            return &slot->view;
        }
        if (!free_slot && !slot->in_use) {
            free_slot = slot;
        }
    }

    if (!free_slot) {
        /* Pool exhausted: recycle slot 0. Before overwriting it, release any
         * temporary override the evicted view still owns. Ownership in
         * temp_slots is tracked by view pointer, so without this the evicted
         * element's temp slot would be leaked forever (in_use, but its owner
         * address is about to be reused for a different element) and could
         * later be aliased by owner-pointer to the new element that reuses
         * this same view address. A caller still holding a stale pointer for
         * the evicted (array, index) will observe the override cleared along
         * with the value being redirected to the new element - an inherent
         * limitation of holding a pointer past pool eviction. */
        free_slot = &array_view_pool[0];
        clear_temporary_locked(&free_slot->view);
    }

    free_slot->in_use = true;
    free_slot->view = *array_descriptor;
    free_slot->view.array_index = index;
    free_slot->view.array_state = array_descriptor->array_state;
    free_slot->view.default_value = NULL;
    /* Redirect the copied descriptor's state pointer away from the array
     * descriptor's own state block to this view's embedded one, then reset
     * it (P4). */
    free_slot->state = (struct zmk_custom_setting_state){.temp_slot = -1};
    free_slot->view.state = &free_slot->state;
    return &free_slot->view;
}

/*
 * BEHAVIOR values are stored/cached the same way ZMK's own keymap settings
 * storage persists behavior bindings (struct zmk_behavior_binding_setting in
 * dependencies/zmk's app/src/keymap.c): a packed struct with trailing
 * all-zero params truncated off before writing, since most bindings only use
 * one or zero params. Reading zero-initializes the struct first, so a
 * truncated stored value naturally decodes back to param1/param2 == 0.
 */
struct zmk_custom_setting_behavior_storage {
    zmk_behavior_local_id_t behavior_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

#define BEHAVIOR_VALUE_ENCODED_MAX_SIZE sizeof(struct zmk_custom_setting_behavior_storage)

static int encode_behavior_value(const struct zmk_custom_setting_behavior_value *behavior,
                                 uint8_t *out, size_t out_capacity, size_t *out_size) {
    if (out_capacity < BEHAVIOR_VALUE_ENCODED_MAX_SIZE) {
        return -EMSGSIZE;
    }

    struct zmk_custom_setting_behavior_storage storage = {
        .behavior_id = (zmk_behavior_local_id_t)behavior->behavior_id,
        .param1 = behavior->param1,
        .param2 = behavior->param2,
    };

    size_t len = sizeof(storage);
    if (storage.param2 == 0) {
        len -= sizeof(storage.param2);

        if (storage.param1 == 0) {
            len -= sizeof(storage.param1);
        }
    }

    memcpy(out, &storage, len);
    *out_size = len;
    return 0;
}

static int decode_behavior_value(const uint8_t *data, size_t size,
                                 struct zmk_custom_setting_behavior_value *behavior) {
    if (size > BEHAVIOR_VALUE_ENCODED_MAX_SIZE) {
        return -EMSGSIZE;
    }

    struct zmk_custom_setting_behavior_storage storage = {0};
    memcpy(&storage, data, size);

    behavior->behavior_id = storage.behavior_id;
    behavior->param1 = storage.param1;
    behavior->param2 = storage.param2;
    return 0;
}

/* Shared scratch buffer for encode_behavior_value() output. Safe as a single
 * instance: every value_to_storage() caller holds settings_lock (see
 * temp_scratch_value above for the same pattern). */
static uint8_t behavior_encode_scratch[BEHAVIOR_VALUE_ENCODED_MAX_SIZE];

static void value_from_raw(struct zmk_custom_setting_value *dest,
                           enum zmk_custom_setting_value_type type, const void *data, size_t size) {
    dest->type = type;
    switch (type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        dest->size = size;
        memcpy(dest->bytes_value, data, size);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        memcpy(dest->string_value, data, size);
        dest->string_value[size] = '\0';
        dest->size = size;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        memcpy(&dest->int32_value, data, sizeof(dest->int32_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        memcpy(&dest->bool_value, data, sizeof(dest->bool_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        decode_behavior_value(data, size, &dest->behavior_value);
        break;
    default:
        break;
    }
}

static int temp_slot_find(const struct zmk_custom_setting *setting) {
    for (size_t i = 0; i < ARRAY_SIZE(temp_slots); i++) {
        if (temp_slots[i].in_use && temp_slots[i].owner == setting) {
            return (int)i;
        }
    }

    return -1;
}

static int temp_slot_alloc(const struct zmk_custom_setting *setting) {
    int existing = temp_slot_find(setting);
    if (existing >= 0) {
        return existing;
    }

    for (size_t i = 0; i < ARRAY_SIZE(temp_slots); i++) {
        if (!temp_slots[i].in_use) {
            temp_slots[i] =
                (struct zmk_custom_settings_temp_slot){.in_use = true, .owner = setting};
            return (int)i;
        }
    }

    return -1;
}

static void temp_slot_free(int slot) {
    if (slot >= 0 && (size_t)slot < ARRAY_SIZE(temp_slots)) {
        temp_slots[slot] = (struct zmk_custom_settings_temp_slot){0};
    }
}

/* Clear an active temporary override, if any, and release its pool slot.
 * Use this instead of clearing the TEMPORARY_ACTIVE state flag directly so
 * the pool slot is not leaked. */
static void clear_temporary_locked(const struct zmk_custom_setting *setting) {
    struct zmk_custom_setting_state *state = setting->state;
    /* Check slot ownership (not just state->temp_slot >= 0) so a state
     * block that was never initialized to temp_slot = -1 (e.g. a
     * zero-initialized view-pool entry) cannot free a slot actually owned
     * by a different setting. */
    if (state->temp_slot >= 0 && (size_t)state->temp_slot < ARRAY_SIZE(temp_slots) &&
        temp_slots[state->temp_slot].owner == setting) {
        temp_slot_free(state->temp_slot);
    }
    state->temp_slot = -1;
    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE, false);
}

static bool *array_dirty_slot(const struct zmk_custom_setting *setting) {
    return &setting->array_state->dirty[setting->array_index];
}

static bool *array_has_persistent_slot(const struct zmk_custom_setting *setting) {
    return &setting->array_state->has_persistent[setting->array_index];
}

static bool setting_is_dirty(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        return *array_dirty_slot(setting);
    }

    return state_flag(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY);
}

static void set_setting_dirty(const struct zmk_custom_setting *setting, bool dirty) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        *array_dirty_slot(setting) = dirty;
        return;
    }

    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY, dirty);
}

static bool setting_has_persistent_value(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        return *array_has_persistent_slot(setting);
    }

    return state_flag(setting, ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT);
}

static void set_setting_has_persistent_value(const struct zmk_custom_setting *setting, bool value) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        *array_has_persistent_slot(setting) = value;
        return;
    }

    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT, value);
}

/*
 * Materialize the current MEMORY value (never a temporary override) as a
 * fixed carrier, or NULL when a blob payload does not fit the carrier
 * (callers must then use zmk_custom_setting_read_into / the raw blob path
 * instead). Array elements are stored as carriers already and are returned
 * in place; a scalar or a carrier-sized blob value is materialized into the
 * shared effective_scratch_value (settings_lock held, like every scratch in
 * this file). Simplification P4: this replaces the pre-P4 memory_value_slot()
 * - there is no embedded carrier on the descriptor to point at anymore.
 */
static const struct zmk_custom_setting_value *
memory_value_locked(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        return &setting->array_state->values[setting->array_index];
    }

    const struct zmk_custom_setting_state *state = setting->state;

    if (setting_uses_blob_store(setting)) {
        if (state->blob.size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return NULL;
        }
        /* A pooled setting with no region yet (blob.data == NULL) holds an
         * empty value - value_from_raw must not be handed a NULL source
         * pointer even for a zero-length copy. */
        value_from_raw(&effective_scratch_value, setting->value_type,
                       state->blob.data != NULL ? state->blob.data : (const uint8_t *)"",
                       state->blob.size);
        return &effective_scratch_value;
    }

    effective_scratch_value = (struct zmk_custom_setting_value){.type = setting->value_type};
    switch (setting->value_type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        effective_scratch_value.int32_value = state->int32_value;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        effective_scratch_value.bool_value = state->bool_value;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        effective_scratch_value.behavior_value = state->behavior;
        break;
    default:
        break;
    }
    return &effective_scratch_value;
}

/*
 * Return the effective value as a fixed carrier, or NULL when the value is a
 * blob payload that does not fit the carrier (callers must then use
 * zmk_custom_setting_read_into / the chunked RPC instead). A temporary
 * override (always carrier-sized) takes precedence over the memory value.
 */
static const struct zmk_custom_setting_value *
effective_value(const struct zmk_custom_setting *setting) {
    if (setting_temporary_active(setting) && setting->state->temp_slot >= 0) {
        const struct zmk_custom_settings_temp_slot *slot = &temp_slots[setting->state->temp_slot];
        value_from_raw(&temp_scratch_value, slot->type, slot->data, slot->size);
        return &temp_scratch_value;
    }

    return memory_value_locked(setting);
}

static void raise_setting_changed(const struct zmk_custom_setting *setting,
                                  enum zmk_custom_setting_changed_kind kind) {
    raise_zmk_custom_setting_changed((struct zmk_custom_setting_changed){
        .setting = setting,
        .kind = kind,
        .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
    });
}

bool zmk_custom_setting_matches(const struct zmk_custom_setting *setting,
                                const char *custom_subsystem_id, const char *key,
                                const char *key_prefix) {
    const char *public_key = zmk_custom_setting_public_key(setting);

    if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
        strncmp(setting->custom_subsystem_id, custom_subsystem_id,
                CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
        return false;
    }

    if (key && key[0] != '\0') {
        bool key_matches_storage =
            strncmp(setting->key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0;
        bool key_matches_public =
            strncmp(public_key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0;
        if (!key_matches_storage && !key_matches_public) {
            return false;
        }
    }

    if (key_prefix && key_prefix[0] != '\0') {
        bool prefix_matches_storage = strncmp(setting->key, key_prefix, strlen(key_prefix)) == 0;
        bool prefix_matches_public = strncmp(public_key, key_prefix, strlen(key_prefix)) == 0;
        if (!prefix_matches_storage && !prefix_matches_public) {
            return false;
        }
    }

    return true;
}

const struct zmk_custom_setting *zmk_custom_setting_find(const char *custom_subsystem_id,
                                                         const char *key) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
            strncmp(setting->custom_subsystem_id, custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
            continue;
        }

        if (key && key[0] != '\0' &&
            strncmp(setting->key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0) {
            return setting;
        }
    }

    if (!key || key[0] == '\0') {
        return NULL;
    }

    /* Simplification P3: keyspace slots are no longer reachable through
     * ZMK_CUSTOM_SETTING_FOREACH (they were, via the now-deleted general
     * runtime-registration list) - check keyspaces explicitly so a
     * user-created entry stays find-able by its literal user key. This is
     * one of the audited FOREACH call sites from
     * docs/design/simplification-redesign.md §5. */
    struct zmk_custom_setting_keyspace *keyspace =
        zmk_custom_settings_keyspace_find_for_key(custom_subsystem_id, key);
    return keyspace ? zmk_custom_setting_keyspace_find(keyspace, key) : NULL;
}

const struct zmk_custom_setting *zmk_custom_setting_find_array(const char *custom_subsystem_id,
                                                               const char *key) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!zmk_custom_setting_is_array(setting)) {
            continue;
        }

        if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
            strncmp(setting->custom_subsystem_id, custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
            continue;
        }

        if (strncmp(setting->array_key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0) {
            return setting;
        }
    }

    return NULL;
}

const struct zmk_custom_setting *
zmk_custom_setting_find_array_element(const char *custom_subsystem_id, const char *key,
                                      uint32_t index) {
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array(custom_subsystem_id, key);
    if (!array_setting || index >= array_setting->array_state->max_size) {
        return NULL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    struct zmk_custom_setting *view = array_view_acquire(array_setting, index);
    k_mutex_unlock(&settings_lock);

    return view;
}

/* Scratch destination for a keyspace slot's decoded user key (see
 * zmk_custom_setting_public_key below). Safe as a single shared instance:
 * populated and consumed synchronously by the caller before any other
 * settings_lock-holding operation can run on this thread, matching the
 * temp_scratch_value/effective_scratch_value pattern already used in this
 * file - not to be treated as valid past the immediate call. */
static char keyspace_public_key_scratch[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];

const char *zmk_custom_setting_public_key(const struct zmk_custom_setting *setting) {
    if (setting->_keyspace) {
        k_mutex_lock(&settings_lock, K_FOREVER);
        size_t key_len = keyspace_blob_key_len_locked(setting);
        key_len = MIN(key_len, sizeof(keyspace_public_key_scratch) - 1);
        if (setting->state->blob.data != NULL && key_len > 0) {
            memcpy(keyspace_public_key_scratch, setting->state->blob.data, key_len);
        }
        keyspace_public_key_scratch[key_len] = '\0';
        k_mutex_unlock(&settings_lock);
        return keyspace_public_key_scratch;
    }

    return zmk_custom_setting_is_array(setting) ? setting->array_key : setting->key;
}

bool zmk_custom_setting_is_array(const struct zmk_custom_setting *setting) {
    return setting && setting->array_key != NULL;
}

static int validate_array_size(const struct zmk_custom_setting *setting, uint32_t array_size) {
    if (!zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    return array_size <= setting->array_state->max_size ? 0 : -ERANGE;
}

/* Clear any active temporary override on array elements at or past
 * new_size. Only live view-pool slots for this array can have an active
 * override (temp_slot lives on the view instance, not in array_state), so
 * this scans the bounded pool instead of the (potentially much larger)
 * array itself. */
static void
clear_temporary_past_size_locked(const struct zmk_custom_setting_array_state *array_state,
                                 uint32_t new_size) {
    for (size_t i = 0; i < ARRAY_SIZE(array_view_pool); i++) {
        struct zmk_custom_setting_array_view_slot *slot = &array_view_pool[i];
        if (slot->in_use && slot->view.array_state == array_state &&
            slot->view.array_index >= new_size) {
            clear_temporary_locked(&slot->view);
        }
    }
}

static void set_array_memory_size_locked(const struct zmk_custom_setting *array_element,
                                         uint32_t array_size) {
    struct zmk_custom_setting_array_state *array_state = array_element->array_state;

    array_state->size = array_size;
    clear_temporary_past_size_locked(array_state, array_size);
}

static void set_array_persistent_size_locked(const struct zmk_custom_setting *array_element,
                                             uint32_t array_size) {
    array_element->array_state->persistent_size = array_size;
}

uint32_t zmk_custom_setting_array_size(const struct zmk_custom_setting *setting) {
    if (!zmk_custom_setting_is_array(setting)) {
        return 0;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    uint32_t array_size = setting->array_state->size;
    k_mutex_unlock(&settings_lock);

    return array_size;
}

uint32_t zmk_custom_setting_array_max_size(const struct zmk_custom_setting *setting) {
    return zmk_custom_setting_is_array(setting) ? setting->array_state->max_size : 0;
}

/* Validate a BEHAVIOR value's behaviorId/param1/param2 against the target
 * behavior's real ZMK parameter metadata (CONFIG_ZMK_BEHAVIOR_METADATA),
 * mirroring how validate_behavior_id_constraint below resolves a behavior
 * local id, but additionally checking param1/param2 through
 * zmk_behavior_validate_binding instead of accepting any uint32_t. */
static int validate_behavior_value(const struct zmk_custom_setting_behavior_value *behavior) {
    if (behavior->behavior_id >= UINT16_MAX) {
        return -ERANGE;
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    const char *behavior_dev_name = zmk_behavior_find_behavior_name_from_local_id(
        (zmk_behavior_local_id_t)behavior->behavior_id);
    if (!behavior_dev_name) {
        return -EINVAL;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = behavior_dev_name,
        .param1 = behavior->param1,
        .param2 = behavior->param2,
    };
    return zmk_behavior_validate_binding(&binding);
#else
    return -ENOTSUP;
#endif
}

static int value_type_validate(const struct zmk_custom_setting *setting,
                               const struct zmk_custom_setting_value *value) {
    if (setting->value_type != value->type) {
        return -EINVAL;
    }

    switch (value->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        return value->size <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? 0 : -EMSGSIZE;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        return value->size <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE ? 0 : -EMSGSIZE;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        return validate_behavior_value(&value->behavior_value);
    default:
        return -EINVAL;
    }
}

static int compare_values(const struct zmk_custom_setting_value *a,
                          const struct zmk_custom_setting_value *b) {
    if (a->type != b->type) {
        return 0;
    }

    switch (a->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        return (a->int32_value > b->int32_value) - (a->int32_value < b->int32_value);
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        return (a->bool_value > b->bool_value) - (a->bool_value < b->bool_value);
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        return strncmp(a->string_value, b->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES: {
        size_t len = MIN(a->size, b->size);
        int cmp = memcmp(a->bytes_value, b->bytes_value, len);
        if (cmp != 0) {
            return cmp;
        }
        return (a->size > b->size) - (a->size < b->size);
    }
    default:
        return 0;
    }
}

static int validate_int32_value(const struct zmk_custom_setting_value *value,
                                int32_t *int32_value) {
    if (value->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        return -EINVAL;
    }

    *int32_value = value->int32_value;
    return 0;
}

static int validate_hid_usage_constraint(const struct zmk_custom_setting_constraint *constraint,
                                         const struct zmk_custom_setting_value *value) {
    int32_t int32_value;
    int ret = validate_int32_value(value, &int32_value);
    if (ret < 0) {
        return ret;
    }
    if (int32_value < 0) {
        return -ERANGE;
    }

    uint32_t usage = (uint32_t)int32_value;
    uint32_t usage_page = ZMK_HID_USAGE_PAGE(usage);
    uint32_t usage_id = ZMK_HID_USAGE_ID(usage);

    if (usage_page == 0U) {
        usage_page = constraint->hid_usage.usage_page;
        usage_id = usage;
    }

    if (usage_page != constraint->hid_usage.usage_page ||
        usage_id < constraint->hid_usage.usage_min || usage_id > constraint->hid_usage.usage_max) {
        return -ERANGE;
    }

    return 0;
}

static int validate_layer_id_constraint(const struct zmk_custom_setting_value *value) {
    int32_t layer_id;
    int ret = validate_int32_value(value, &layer_id);
    if (ret < 0) {
        return ret;
    }

    return layer_id >= 0 && layer_id < ZMK_KEYMAP_LAYERS_LEN ? 0 : -ERANGE;
}

static int validate_behavior_id_constraint(const struct zmk_custom_setting_value *value) {
    int32_t behavior_id;
    int ret = validate_int32_value(value, &behavior_id);
    if (ret < 0) {
        return ret;
    }
    if (behavior_id < 0 || behavior_id >= UINT16_MAX) {
        return -ERANGE;
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    return zmk_behavior_find_behavior_name_from_local_id((zmk_behavior_local_id_t)behavior_id)
               ? 0
               : -EINVAL;
#else
    return -ENOTSUP;
#endif
}

int zmk_custom_setting_validate(const struct zmk_custom_setting *setting,
                                const struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    int ret = value_type_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    for (size_t c = 0; c < setting->constraints_count; c++) {
        const struct zmk_custom_setting_constraint *constraint = &setting->constraints[c];

        switch (constraint->type) {
        case ZMK_CUSTOM_SETTING_CONSTRAINT_NONE:
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE:
            ret = validate_hid_usage_constraint(constraint, value);
            if (ret < 0) {
                return ret;
            }
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID:
            ret = validate_layer_id_constraint(value);
            if (ret < 0) {
                return ret;
            }
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID:
            ret = validate_behavior_id_constraint(value);
            if (ret < 0) {
                return ret;
            }
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE:
            if (compare_values(value, &constraint->range.min) < 0 ||
                compare_values(value, &constraint->range.max) > 0) {
                return -ERANGE;
            }
            break;
        case ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS: {
            bool matched = false;
            for (size_t i = 0; i < constraint->options.count; i++) {
                if (value_equals(value, &constraint->options.values[i])) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return -EINVAL;
            }
            break;
        }
        default:
            return -EINVAL;
        }
    }

    return 0;
}

int zmk_custom_setting_set_default(const struct zmk_custom_setting *setting,
                                   const struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    /* Array elements have per-index defaults from array_state->defaults,
     * set once at registration time (ZMK_CUSTOM_SETTING_ARRAY_DEFINE); there
     * is no runtime-replaceable default for an array element/view. */
    if (zmk_custom_setting_is_array(setting)) {
        return -ENOTSUP;
    }

    int ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    /* Simplification P4: the descriptor (and its default_value pointer) is
     * const/flash-resident, so the replacement default lives on the RAM
     * state block instead and setting_default_value() consults it first
     * everywhere a default is read (init, discard, reset). Same documented
     * semantics as before. */
    setting->state->default_override = value;
    /* No persisted value has been loaded and no in-memory write has happened
     * yet, so the current memory value (whatever it was materialized to, or
     * even its zero-initialized pre-init state) still represents "unset" -
     * refresh it too. This makes the call safe regardless of whether it runs
     * before or after this module's own registry init, as long as it is
     * before settings_load() (i.e. from any SYS_INIT). */
    if (!setting_has_persistent_value(setting) &&
        !state_flag(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY)) {
        int store_ret = store_scalar_value_locked(setting, value);
        if (store_ret < 0) {
            k_mutex_unlock(&settings_lock);
            return store_ret;
        }
    }
    k_mutex_unlock(&settings_lock);

    return 0;
}

static int setting_storage_name(const struct zmk_custom_setting *setting, char *name,
                                size_t name_size) {
    int ret;

    /* Array element views do not carry a baked-in "key/index" string (there
     * is no per-element registration anymore); build the storage name from
     * the shared array_key plus this view's array_index instead. */
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        ret = snprintf(name, name_size, SETTINGS_SUBTREE "/%s/%s/%u", setting->custom_subsystem_id,
                       setting->array_key, setting->array_index);
    } else {
        ret = snprintf(name, name_size, SETTINGS_SUBTREE "/%s/%s", setting->custom_subsystem_id,
                       setting->key);
    }
    if (ret < 0 || ret >= name_size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

static int array_size_storage_name(const struct zmk_custom_setting *setting, char *name,
                                   size_t name_size) {
    if (!zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    int ret = snprintf(name, name_size, SETTINGS_SUBTREE "/%s/%s/%s", setting->custom_subsystem_id,
                       setting->array_key, ARRAY_SIZE_STORAGE_KEY);
    if (ret < 0 || ret >= name_size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

static int value_to_storage(const struct zmk_custom_setting_value *value, const void **data,
                            size_t *len) {
    switch (value->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        *data = value->bytes_value;
        *len = value->size;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        *data = &value->int32_value;
        *len = sizeof(value->int32_value);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        *data = &value->bool_value;
        *len = sizeof(value->bool_value);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        *data = value->string_value;
        *len = bounded_strlen(value->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR: {
        size_t size;
        int ret = encode_behavior_value(&value->behavior_value, behavior_encode_scratch,
                                        sizeof(behavior_encode_scratch), &size);
        if (ret < 0) {
            return ret;
        }
        *data = behavior_encode_scratch;
        *len = size;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

/* Delete on-flash records for slots at or past array_size. Operates
 * directly on the descriptor's array_state buffer (O(max_size), bounded by
 * the array's own max element count) instead of walking the full setting
 * registry looking for "sibling" registrations. */
static int delete_inactive_array_values_locked(const struct zmk_custom_setting *array_descriptor,
                                               uint32_t array_size) {
    struct zmk_custom_setting_array_state *array_state = array_descriptor->array_state;

    for (uint32_t index = array_size; index < array_state->max_size; index++) {
        if (!array_state->has_persistent[index]) {
            continue;
        }

        struct zmk_custom_setting *view =
            array_view_acquire((struct zmk_custom_setting *)array_descriptor, index);
        char name[SETTINGS_MAX_NAME_LEN];
        int ret = setting_storage_name(view, name, sizeof(name));
        if (ret < 0) {
            return ret;
        }

        ret = settings_delete(name);
        if (ret != 0 && ret != -ENOENT) {
            return ret;
        }

        array_state->has_persistent[index] = false;
        array_state->dirty[index] = true;
    }

    return 0;
}

/* Save all active elements (indices [0, array_size)) plus the "_size"
 * marker, then delete any now-inactive elements. Operates directly on the
 * descriptor's array_state buffer instead of walking the full setting
 * registry for "sibling" element registrations - this is the O(N)-elimination
 * point for saving arrays. */
static int save_array_locked(const struct zmk_custom_setting *array_descriptor) {
    char size_name[SETTINGS_MAX_NAME_LEN];
    int ret = array_size_storage_name(array_descriptor, size_name, sizeof(size_name));
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_array_state *array_state = array_descriptor->array_state;
    uint32_t array_size = array_state->size;
    ret = settings_save_one(size_name, &array_size, sizeof(array_size));
    if (ret < 0) {
        return ret;
    }

    for (uint32_t index = 0; index < array_size; index++) {
        struct zmk_custom_setting *view =
            array_view_acquire((struct zmk_custom_setting *)array_descriptor, index);
        char name[SETTINGS_MAX_NAME_LEN];
        ret = setting_storage_name(view, name, sizeof(name));
        if (ret < 0) {
            return ret;
        }

        const void *data;
        size_t len;
        ret = value_to_storage(&array_state->values[index], &data, &len);
        if (ret < 0) {
            return ret;
        }

        ret = settings_save_one(name, data, len);
        if (ret < 0) {
            return ret;
        }

        array_state->has_persistent[index] = true;
        array_state->dirty[index] = false;
    }

    set_array_persistent_size_locked(array_descriptor, array_size);
    return delete_inactive_array_values_locked(array_descriptor, array_size);
}

static int save_setting_locked(const struct zmk_custom_setting *setting) {
    /* An array descriptor or any index view of it saves the whole array
     * (all active elements plus the "_size" marker) - see save_array_locked.
     * This is also correct for a single element view (e.g. the "tail"
     * element passed by zmk_custom_setting_array_pop_back): saving the
     * whole array is a superset of saving one element and keeps this
     * function's callers (zmk_custom_setting_save, write_value_locked)
     * simple. */
    if (zmk_custom_setting_is_array(setting)) {
        return save_array_locked(setting);
    }

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(setting, name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    const void *data;
    size_t len;
    if (setting_uses_blob_store(setting)) {
        /* A BYTES/STRING payload is stored raw; persist it directly from
         * the blob store (any size). A pooled setting with no region (empty
         * value) has blob.data == NULL - settings_save_one(name, NULL, 0)
         * means "delete this record" to the settings subsystem, not "save
         * an empty value", so substitute a valid (never dereferenced past
         * its 0-byte length) pointer. */
        data = setting->state->blob.data != NULL ? setting->state->blob.data : (const void *)"";
        len = setting->state->blob.size;
    } else {
        const struct zmk_custom_setting_value *memory = memory_value_locked(setting);
        if (!memory) {
            return -EMSGSIZE;
        }
        ret = value_to_storage(memory, &data, &len);
        if (ret < 0) {
            return ret;
        }
    }

    ret = settings_save_one(name, data, len);
    if (ret < 0) {
        return ret;
    }

    set_setting_has_persistent_value(setting, true);
    set_setting_dirty(setting, false);
    return 0;
}

int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                            struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    if (setting->_keyspace) {
        return keyspace_read_payload(setting, value);
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index >= setting->array_state->size) {
        k_mutex_unlock(&settings_lock);
        return -ENOENT;
    }

    const struct zmk_custom_setting_value *effective = effective_value(setting);
    if (!effective) {
        /* Large value that does not fit the fixed carrier - the caller must
         * use zmk_custom_setting_read_into (or, over RPC, the ordinary
         * streamed GetSetting/ListSettings response - see
         * custom_settings_handler.c) instead of the fixed-carrier read. */
        k_mutex_unlock(&settings_lock);
        return -EMSGSIZE;
    }
    copy_value(value, effective);
    k_mutex_unlock(&settings_lock);

    return 0;
}

int zmk_custom_setting_read_by_key(const char *custom_subsystem_id, const char *key,
                                   struct zmk_custom_setting_value *value) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find(custom_subsystem_id, key);
    if (!setting) {
        return -ENOENT;
    }

    return zmk_custom_setting_read(setting, value);
}

static int convert_rpc_bytes_value(const struct zmk_custom_setting *setting,
                                   const struct zmk_custom_setting_value *src,
                                   struct zmk_custom_setting_value *dest,
                                   zmk_custom_setting_rpc_bytes_converter_t converter) {
    if (!setting || !src || !dest) {
        return -EINVAL;
    }

    /* A keyspace slot's own value_type is always BYTES internally (the
     * opaque blob); the PRESENTED type - what `src`/`dest` here are actually
     * typed as (the PAYLOAD, not the blob) - is the owning keyspace's
     * declared value_type. */
    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;
    enum zmk_custom_setting_value_type presented_type =
        keyspace ? keyspace->value_type : setting->value_type;

    if (presented_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
        src->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES || converter == NULL) {
        copy_value(dest, src);
        return 0;
    }

    *dest = (struct zmk_custom_setting_value){
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    };
    size_t dest_size = 0;
    int ret = converter(setting, src->bytes_value, src->size, dest->bytes_value, &dest_size,
                        sizeof(dest->bytes_value));
    if (ret < 0) {
        return ret;
    }
    if (dest_size > sizeof(dest->bytes_value)) {
        return -EMSGSIZE;
    }

    dest->size = dest_size;
    return 0;
}

int zmk_custom_setting_serialize_rpc_value(const struct zmk_custom_setting *setting,
                                           const struct zmk_custom_setting_value *internal_value,
                                           struct zmk_custom_setting_value *rpc_value) {
    if (!setting) {
        return convert_rpc_bytes_value(setting, internal_value, rpc_value, NULL);
    }
    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;
    return convert_rpc_bytes_value(setting, internal_value, rpc_value,
                                   keyspace ? keyspace->rpc_serializer : setting->rpc_serializer);
}

int zmk_custom_setting_deserialize_rpc_value(const struct zmk_custom_setting *setting,
                                             const struct zmk_custom_setting_value *rpc_value,
                                             struct zmk_custom_setting_value *internal_value) {
    if (!setting) {
        return convert_rpc_bytes_value(setting, rpc_value, internal_value, NULL);
    }
    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;
    return convert_rpc_bytes_value(setting, rpc_value, internal_value,
                                   keyspace ? keyspace->rpc_deserializer
                                            : setting->rpc_deserializer);
}

int zmk_custom_setting_read_array_by_key(const char *custom_subsystem_id, const char *key,
                                         uint32_t index, struct zmk_custom_setting_value *value) {
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array_element(custom_subsystem_id, key, index);
    if (!setting) {
        return -ENOENT;
    }

    return zmk_custom_setting_read(setting, value);
}

/* Store `value` as `setting`'s in-memory value, routing to the right
 * backing storage: array element carrier, blob store, or the inline scalar
 * state union. Caller must hold settings_lock. */
static int store_memory_value_locked(const struct zmk_custom_setting *setting,
                                     const struct zmk_custom_setting_value *value) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        copy_value(&setting->array_state->values[setting->array_index], value);
        return 0;
    }

    return store_scalar_value_locked(setting, value);
}

/* Apply a write in the selected mode to an already-resolved, in-range
 * setting. Caller must hold settings_lock. */
static int write_value_locked(const struct zmk_custom_setting *setting,
                              const struct zmk_custom_setting_value *value,
                              enum zmk_custom_setting_write_mode mode) {
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY: {
        const void *data;
        size_t size;
        int ret = value_to_storage(value, &data, &size);
        if (ret < 0) {
            return ret;
        }
        if (size > sizeof(temp_slots[0].data)) {
            return -EMSGSIZE;
        }

        int slot = temp_slot_alloc(setting);
        if (slot < 0) {
            return -EBUSY;
        }

        temp_slots[slot].type = value->type;
        temp_slots[slot].size = size;
        memcpy(temp_slots[slot].data, data, size);
        setting->state->temp_slot = slot;
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE, true);
        return 0;
    }
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY: {
        int ret = store_memory_value_locked(setting, value);
        if (ret < 0) {
            return ret;
        }
        set_setting_dirty(setting, true);
        clear_temporary_locked(setting);
        return 0;
    }
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST: {
        int ret = store_memory_value_locked(setting, value);
        if (ret < 0) {
            return ret;
        }
        clear_temporary_locked(setting);
        return save_setting_locked(setting);
    }
    default:
        return -EINVAL;
    }
}

/*
 * Keyspace-agnostic "write this exact typed value to this exact setting"
 * primitive - today's zmk_custom_setting_write body prior to simplification
 * P3. Used directly by the public zmk_custom_setting_write for a normal
 * setting, AND by write_bytes_raw's small-carrier tail (further down) to
 * store an already-fully-assembled keyspace slot blob verbatim - that second
 * caller must bypass the public zmk_custom_setting_write (see below) since
 * `value` there already IS the blob (typed BYTES), not a payload needing
 * re-encoding.
 */
static int write_scalar_value(const struct zmk_custom_setting *setting,
                              const struct zmk_custom_setting_value *value,
                              enum zmk_custom_setting_write_mode mode) {
    int ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);

    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index >= setting->array_state->size) {
        ret = -ENOENT;
        goto unlock;
    }

    ret = write_value_locked(setting, value, mode);

unlock:
    k_mutex_unlock(&settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_write(const struct zmk_custom_setting *setting,
                             const struct zmk_custom_setting_value *value,
                             enum zmk_custom_setting_write_mode mode) {
    if (!setting || !value) {
        return -EINVAL;
    }

    if (setting->_keyspace) {
        /* `value` here is the PAYLOAD, typed as keyspace->value_type - not
         * the slot's own (always BYTES) blob representation. Validate it
         * against the keyspace's declared type/constraints/max_size, then
         * re-encode blob(current key, new payload) and write that via the
         * normal pooled-BYTES path (keyspace_write_blob's NULL `key` means
         * "reuse the slot's current key" - see the keyspace design comment
         * in the header). */
        int ret = keyspace_validate_payload(setting->_keyspace, value);
        if (ret < 0) {
            return ret;
        }
        return keyspace_write_blob(setting, NULL, value, mode);
    }

    return write_scalar_value(setting, value, mode);
}

int zmk_custom_setting_write_by_key(const char *custom_subsystem_id, const char *key,
                                    const struct zmk_custom_setting_value *value,
                                    enum zmk_custom_setting_write_mode mode) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find(custom_subsystem_id, key);
    if (!setting) {
        return -ENOENT;
    }

    return zmk_custom_setting_write(setting, value, mode);
}

int zmk_custom_setting_write_array_by_key(const char *custom_subsystem_id, const char *key,
                                          uint32_t index,
                                          const struct zmk_custom_setting_value *value,
                                          enum zmk_custom_setting_write_mode mode) {
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array_element(custom_subsystem_id, key, index);
    if (!setting) {
        return -ENOENT;
    }

    return zmk_custom_setting_write(setting, value, mode);
}

int zmk_custom_setting_write_array_element(const struct zmk_custom_setting *const_setting,
                                           const struct zmk_custom_setting_value *value,
                                           uint32_t array_size,
                                           enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !value) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;
    if (!zmk_custom_setting_is_array(setting) || setting->array_index >= array_size) {
        return -EINVAL;
    }

    int ret = validate_array_size(setting, array_size);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    set_array_memory_size_locked(setting, array_size);

    ret = write_value_locked(setting, value, mode);

    k_mutex_unlock(&settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_array_push_back(const struct zmk_custom_setting *setting,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode) {
    if (!setting || !value || !zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    uint32_t array_size = setting->array_state->size;
    uint32_t array_max_size = setting->array_state->max_size;
    k_mutex_unlock(&settings_lock);

    if (array_size >= array_max_size) {
        return -ERANGE;
    }

    const struct zmk_custom_setting *tail = zmk_custom_setting_find_array_element(
        setting->custom_subsystem_id, setting->array_key, array_size);
    if (!tail) {
        return -ENOENT;
    }

    return zmk_custom_setting_write_array_element(tail, value, array_size + 1, mode);
}

int zmk_custom_setting_array_pop_back(const struct zmk_custom_setting *const_setting,
                                      struct zmk_custom_setting_value *value,
                                      enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !zmk_custom_setting_is_array(const_setting)) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&settings_lock, K_FOREVER);
    uint32_t array_size = setting->array_state->size;
    if (array_size == 0) {
        k_mutex_unlock(&settings_lock);
        return -ENOENT;
    }
    k_mutex_unlock(&settings_lock);

    const struct zmk_custom_setting *tail_const = zmk_custom_setting_find_array_element(
        setting->custom_subsystem_id, setting->array_key, array_size - 1);
    if (!tail_const) {
        return -ENOENT;
    }

    struct zmk_custom_setting *tail = (struct zmk_custom_setting *)tail_const;

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (value) {
        copy_value(value, effective_value(tail));
    }

    set_array_memory_size_locked(setting, array_size - 1);
    clear_temporary_locked(tail);

    int ret = 0;
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        ret = save_setting_locked(tail);
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY:
        break;
    default:
        ret = -EINVAL;
        break;
    }

    k_mutex_unlock(&settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_array_insert_at(const struct zmk_custom_setting *const_setting,
                                       uint32_t index, const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !value || !zmk_custom_setting_is_array(const_setting)) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;
    int ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    struct zmk_custom_setting_array_state *array_state = setting->array_state;
    uint32_t array_size = array_state->size;

    if (index > array_size) {
        k_mutex_unlock(&settings_lock);
        return -ERANGE;
    }
    if (array_size >= array_state->max_size) {
        k_mutex_unlock(&settings_lock);
        return -ERANGE;
    }

    /* Shift [index, array_size) one slot later over the single contiguous
     * buffer instead of requiring N individual element writes - the whole
     * point of a contiguous backing buffer. Any temp override on a shifted
     * element is cleared (its underlying value moved), matching how
     * set_array_memory_size_locked/clear_temporary_past_size_locked already
     * treat elements that fall out of the valid range on resize. */
    uint32_t move_count = array_size - index;
    if (move_count > 0) {
        memmove(&array_state->values[index + 1], &array_state->values[index],
                move_count * sizeof(array_state->values[0]));
        memmove(&array_state->dirty[index + 1], &array_state->dirty[index],
                move_count * sizeof(array_state->dirty[0]));
        memmove(&array_state->has_persistent[index + 1], &array_state->has_persistent[index],
                move_count * sizeof(array_state->has_persistent[0]));
        clear_temporary_past_size_locked(array_state, index);
    }

    copy_value(&array_state->values[index], value);
    array_state->dirty[index] = true;
    array_state->has_persistent[index] = false;
    array_state->size = array_size + 1;

    struct zmk_custom_setting *view = array_view_acquire(setting, index);
    ret = 0;
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        ret = save_setting_locked(view);
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY:
        /* Insertion always establishes a real memory value; "temporary
         * insert" is not a meaningful combination. */
        ret = -EINVAL;
        break;
    default:
        ret = -EINVAL;
        break;
    }
    k_mutex_unlock(&settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_array_remove_at(const struct zmk_custom_setting *const_setting,
                                       uint32_t index, struct zmk_custom_setting_value *out_value,
                                       enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !zmk_custom_setting_is_array(const_setting)) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&settings_lock, K_FOREVER);
    struct zmk_custom_setting_array_state *array_state = setting->array_state;
    uint32_t array_size = array_state->size;

    if (index >= array_size) {
        k_mutex_unlock(&settings_lock);
        return -ENOENT;
    }

    if (out_value) {
        struct zmk_custom_setting *removed_view = array_view_acquire(setting, index);
        copy_value(out_value, effective_value(removed_view));
    }
    clear_temporary_past_size_locked(array_state, index);

    /* Shift [index + 1, array_size) one slot earlier over the single
     * contiguous buffer instead of requiring N individual element writes. */
    uint32_t move_count = array_size - index - 1;
    if (move_count > 0) {
        memmove(&array_state->values[index], &array_state->values[index + 1],
                move_count * sizeof(array_state->values[0]));
        memmove(&array_state->dirty[index], &array_state->dirty[index + 1],
                move_count * sizeof(array_state->dirty[0]));
        memmove(&array_state->has_persistent[index], &array_state->has_persistent[index + 1],
                move_count * sizeof(array_state->has_persistent[0]));
        clear_temporary_past_size_locked(array_state, array_size - 1);
    }

    array_state->size = array_size - 1;
    array_state->dirty[array_size - 1] = true;

    struct zmk_custom_setting *tail_view = array_view_acquire(setting, array_size - 1);
    int ret = 0;
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        ret = save_setting_locked(tail_view);
        break;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY:
        break;
    default:
        ret = -EINVAL;
        break;
    }
    k_mutex_unlock(&settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_save(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&settings_lock, K_FOREVER);
    clear_temporary_locked(setting);
    int ret = save_setting_locked(setting);
    k_mutex_unlock(&settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_SAVED);
    }

    return ret;
}

/* Discard one array element view's in-memory value back to persisted/
 * default, and (once per call, harmless if repeated) restore the whole
 * array's active length to its persisted length. Called once per active
 * element by apply_scope so zmk_custom_settings_discard_scope's
 * affected_count matches "one per active element", same as the pre-P3
 * per-element registrations. */
static int discard_array_element_locked(struct zmk_custom_setting *view) {
    struct zmk_custom_setting_array_state *array_state = view->array_state;
    uint32_t index = view->array_index;

    set_array_memory_size_locked(view, array_state->persistent_size);

    if (array_state->has_persistent[index]) {
        /* No RAM-resident persistent_value copy is kept; re-read the
         * persisted value from flash straight into the buffer slot.
         * Discard is a rare, explicit user action, so a flash read here is
         * fine. */
        char name[SETTINGS_MAX_NAME_LEN];
        int ret = setting_storage_name(view, name, sizeof(name));
        if (ret == 0) {
            ret = settings_load_subtree(name);
        }
        if (ret < 0) {
            copy_value(&array_state->values[index], &array_state->defaults[index]);
        }
    } else {
        copy_value(&array_state->values[index], &array_state->defaults[index]);
    }
    array_state->dirty[index] = false;
    clear_temporary_locked(view);
    return 0;
}

int zmk_custom_setting_discard(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&settings_lock, K_FOREVER);

    if (zmk_custom_setting_is_array(setting)) {
        /* Called directly on the array descriptor (array_index ==
         * ZMK_CUSTOM_SETTING_ARRAY_NONE): discard every element up to the
         * (about to be restored) persistent size. Called on a single
         * element view (from apply_scope's per-active-element expansion):
         * discard just that element, matching the pre-P3 per-element
         * registration's zmk_custom_setting_discard(element) behavior. */
        if (setting->array_index == ZMK_CUSTOM_SETTING_ARRAY_NONE) {
            struct zmk_custom_setting_array_state *array_state = setting->array_state;
            uint32_t restore_size = array_state->persistent_size;
            for (uint32_t index = 0; index < restore_size; index++) {
                struct zmk_custom_setting *view = array_view_acquire(setting, index);
                discard_array_element_locked(view);
            }
            set_array_memory_size_locked(setting, restore_size);
        } else {
            discard_array_element_locked(setting);
        }
        k_mutex_unlock(&settings_lock);
        raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_DISCARDED);
        return 0;
    }

    if (setting_has_persistent_value(setting)) {
        /* No RAM-resident persistent_value copy is kept; re-read the
         * persisted value from flash straight into the memory store.
         * Discard is a rare, explicit user action, so a flash read here is
         * fine. */
        char name[SETTINGS_MAX_NAME_LEN];
        int ret = setting_storage_name(setting, name, sizeof(name));
        if (ret == 0) {
            ret = settings_load_subtree(name);
        }
        if (ret < 0) {
            apply_scalar_default_locked(setting);
        }
    } else {
        apply_scalar_default_locked(setting);
    }
    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY, false);
    clear_temporary_locked(setting);
    k_mutex_unlock(&settings_lock);

    raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_DISCARDED);
    return 0;
}

/* Reset one array element view's persisted/in-memory value to its
 * compile-time default. Used both directly (whole-array reset) and once per
 * active element by apply_scope. */
static int reset_array_element_locked(struct zmk_custom_setting *view) {
    struct zmk_custom_setting_array_state *array_state = view->array_state;
    uint32_t index = view->array_index;

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(view, name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    ret = settings_delete(name);
    if (ret == -ENOENT) {
        ret = 0;
    }
    if (ret < 0) {
        return ret;
    }

    copy_value(&array_state->values[index], &array_state->defaults[index]);
    array_state->has_persistent[index] = false;
    array_state->dirty[index] = false;
    clear_temporary_locked(view);
    return 0;
}

int zmk_custom_setting_reset(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    if (zmk_custom_setting_is_array(setting)) {
        k_mutex_lock(&settings_lock, K_FOREVER);
        struct zmk_custom_setting_array_state *array_state = setting->array_state;
        int ret = 0;

        if (setting->array_index == ZMK_CUSTOM_SETTING_ARRAY_NONE) {
            /* Whole-array reset: restore every element up to the current
             * active size to its default and erase all persisted state,
             * including the "_size" marker. */
            uint32_t reset_size = array_state->size;
            for (uint32_t index = 0; index < reset_size && ret == 0; index++) {
                struct zmk_custom_setting *view = array_view_acquire(setting, index);
                ret = reset_array_element_locked(view);
            }
            if (ret == 0) {
                char size_name[SETTINGS_MAX_NAME_LEN];
                ret = array_size_storage_name(setting, size_name, sizeof(size_name));
                if (ret == 0) {
                    ret = settings_delete(size_name);
                    if (ret == -ENOENT) {
                        ret = 0;
                    }
                }
            }
            if (ret == 0) {
                set_array_persistent_size_locked(setting, array_state->default_size);
                set_array_memory_size_locked(setting, array_state->default_size);
            }
        } else {
            /* Single active-element reset, as invoked once per element by
             * apply_scope to match the pre-P3 per-element affected_count. */
            ret = reset_array_element_locked(setting);
            if (ret == 0) {
                char size_name[SETTINGS_MAX_NAME_LEN];
                int size_ret = array_size_storage_name(setting, size_name, sizeof(size_name));
                if (size_ret == 0) {
                    size_ret = settings_delete(size_name);
                    if (size_ret != 0 && size_ret != -ENOENT) {
                        ret = size_ret;
                    }
                } else {
                    ret = size_ret;
                }
                if (ret == 0) {
                    set_array_persistent_size_locked(setting, array_state->default_size);
                    set_array_memory_size_locked(setting, array_state->default_size);
                }
            }
        }

        k_mutex_unlock(&settings_lock);
        if (ret == 0) {
            raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_RESET);
        }
        return ret;
    }

    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(setting, name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    ret = settings_delete(name);
    if (ret == -ENOENT) {
        ret = 0;
    }

    if (ret == 0) {
        apply_scalar_default_locked(setting);
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT, false);
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY, false);
        clear_temporary_locked(setting);
    }
    k_mutex_unlock(&settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_RESET);
    }

    return ret;
}

int zmk_custom_setting_rollback_temporary(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&settings_lock, K_FOREVER);
    clear_temporary_locked(setting);
    k_mutex_unlock(&settings_lock);

    raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    return 0;
}

/* Apply callback to one setting, expanding an array descriptor into one
 * callback invocation per currently-active element (via the index-view
 * mechanism) instead of a single call on the descriptor. This preserves the
 * pre-P3 observable behavior where each array element was its own
 * registered struct zmk_custom_setting and so contributed its own count to
 * *affected_count - e.g. zmk_custom_settings_reset_scope("test",
 * "array_value", NULL, &affected_count) is expected to report exactly 3 for
 * a 3-element active array, not 1 for "one array descriptor". Internally
 * this means save/discard/reset each run their whole-array bookkeeping
 * update redundantly once per element, which is harmless and still
 * O(active count), not O(full registry). */
static int apply_scope_to_setting(const struct zmk_custom_setting *setting,
                                  int (*callback)(const struct zmk_custom_setting *),
                                  uint32_t *count, int *first_error) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index == ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        k_mutex_lock(&settings_lock, K_FOREVER);
        uint32_t active_size = setting->array_state->size;
        k_mutex_unlock(&settings_lock);

        for (uint32_t index = 0; index < active_size; index++) {
            const struct zmk_custom_setting *view = zmk_custom_setting_find_array_element(
                setting->custom_subsystem_id, setting->array_key, index);
            if (!view) {
                continue;
            }

            int ret = callback(view);
            if (ret < 0 && *first_error == 0) {
                *first_error = ret;
                continue;
            }
            if (ret == 0) {
                (*count)++;
            }
        }
        return 0;
    }

    int ret = callback(setting);
    if (ret < 0 && *first_error == 0) {
        *first_error = ret;
    } else if (ret == 0) {
        (*count)++;
    }
    return 0;
}

static int apply_scope(const char *custom_subsystem_id, const char *key, const char *key_prefix,
                       uint32_t *affected_count,
                       int (*callback)(const struct zmk_custom_setting *)) {
    uint32_t count = 0;
    int first_error = 0;
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!zmk_custom_setting_matches(setting, custom_subsystem_id, key, key_prefix)) {
            continue;
        }

        apply_scope_to_setting(setting, callback, &count, &first_error);
    }

    /* Simplification P3: keyspace slots are no longer reachable through
     * ZMK_CUSTOM_SETTING_FOREACH (Goal A audit) - walk every keyspace's live
     * slots explicitly so save/discard/reset scope still reaches
     * user-created entries. A slot is never an array, so no expansion step
     * is needed the way apply_scope_to_setting needs one for arrays. */
    ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(keyspace) {
        for (uint32_t i = 0; i < keyspace->max_entries; i++) {
            k_mutex_lock(&settings_lock, K_FOREVER);
            bool in_use = keyspace->slots[i].in_use;
            k_mutex_unlock(&settings_lock);
            if (!in_use) {
                continue;
            }

            const struct zmk_custom_setting *slot_setting = &keyspace->slots[i].setting;
            if (!zmk_custom_setting_matches(slot_setting, custom_subsystem_id, key, key_prefix)) {
                continue;
            }

            apply_scope_to_setting(slot_setting, callback, &count, &first_error);
        }
    }

    if (affected_count) {
        *affected_count = count;
    }

    return first_error;
}

int zmk_custom_settings_save_scope(const char *custom_subsystem_id, const char *key,
                                   const char *key_prefix, uint32_t *affected_count) {
    return apply_scope(custom_subsystem_id, key, key_prefix, affected_count,
                       zmk_custom_setting_save);
}

int zmk_custom_settings_discard_scope(const char *custom_subsystem_id, const char *key,
                                      const char *key_prefix, uint32_t *affected_count) {
    return apply_scope(custom_subsystem_id, key, key_prefix, affected_count,
                       zmk_custom_setting_discard);
}

int zmk_custom_settings_reset_scope(const char *custom_subsystem_id, const char *key,
                                    const char *key_prefix, uint32_t *affected_count) {
    return apply_scope(custom_subsystem_id, key, key_prefix, affected_count,
                       zmk_custom_setting_reset);
}

bool zmk_custom_setting_has_unsaved_value(const struct zmk_custom_setting *setting) {
    if (!setting) {
        return false;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    bool has_unsaved = setting_temporary_active(setting) || setting_is_dirty(setting);
    if (zmk_custom_setting_is_array(setting)) {
        has_unsaved =
            has_unsaved || setting->array_state->size != setting->array_state->persistent_size;
    }
    k_mutex_unlock(&settings_lock);

    return has_unsaved;
}

int zmk_custom_setting_with_value(const struct zmk_custom_setting *setting,
                                  zmk_custom_setting_value_visitor_t visitor, void *user_data) {
    if (!setting || !visitor) {
        return -EINVAL;
    }

    if (setting->_keyspace) {
        /* Present the decoded PAYLOAD, not the raw [key\0][payload] blob.
         * Materialized into a stack carrier (one copy) rather than borrowed
         * zero-copy - acceptable for the rarely-hot visitor API; large
         * payloads still return -EMSGSIZE here like any other >carrier
         * value. */
        struct zmk_custom_setting_value payload;
        int ret = keyspace_read_payload(setting, &payload);
        if (ret < 0) {
            return ret;
        }
        visitor(&payload, user_data);
        return 0;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index >= setting->array_state->size) {
        k_mutex_unlock(&settings_lock);
        return -ENOENT;
    }

    const struct zmk_custom_setting_value *effective = effective_value(setting);
    if (!effective) {
        /* Large value that does not fit the fixed carrier - callers wanting
         * the raw bytes use zmk_custom_setting_read_into (which reads the
         * large store directly) instead. */
        k_mutex_unlock(&settings_lock);
        return -EMSGSIZE;
    }
    visitor(effective, user_data);
    k_mutex_unlock(&settings_lock);

    return 0;
}

struct read_into_context {
    void *buf;
    size_t capacity;
    size_t out_size;
    enum zmk_custom_setting_value_type out_type;
    int ret;
};

static void read_into_visitor(const struct zmk_custom_setting_value *value, void *user_data) {
    struct read_into_context *ctx = user_data;

    const void *data;
    size_t size;
    switch (value->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        data = value->bytes_value;
        size = value->size;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        data = value->string_value;
        size = value->size;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        data = &value->int32_value;
        size = sizeof(value->int32_value);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        data = &value->bool_value;
        size = sizeof(value->bool_value);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        data = &value->behavior_value;
        size = sizeof(value->behavior_value);
        break;
    default:
        ctx->ret = -EINVAL;
        return;
    }

    if (size > ctx->capacity) {
        ctx->ret = -EMSGSIZE;
        return;
    }

    memcpy(ctx->buf, data, size);
    ctx->out_size = size;
    ctx->out_type = value->type;
    ctx->ret = 0;
}

int zmk_custom_setting_read_into(const struct zmk_custom_setting *setting, void *buf,
                                 size_t capacity, size_t *out_size,
                                 enum zmk_custom_setting_value_type *out_type) {
    if (!setting || (!buf && capacity > 0)) {
        return -EINVAL;
    }

    if (setting->_keyspace) {
        return keyspace_read_into(setting, buf, capacity, out_size, out_type);
    }

    /* Blob fast path: read the raw payload straight from the setting's
     * store, so a value larger than the fixed carrier is still readable (the
     * effective_value carrier below cannot represent it). Skipped while a
     * temporary override is active - those are always carrier-sized. */
    k_mutex_lock(&settings_lock, K_FOREVER);
    if (setting_uses_blob_store(setting) && !setting_temporary_active(setting)) {
        size_t size = setting->state->blob.size;
        int ret = 0;
        if (size > capacity) {
            ret = -EMSGSIZE;
        } else {
            /* A pooled setting with no region yet (blob.data == NULL) has
             * size == 0 here; skip the copy rather than pass a NULL source
             * to memcpy. */
            if (size > 0) {
                memcpy(buf, setting->state->blob.data, size);
            }
            if (out_size) {
                *out_size = size;
            }
            if (out_type) {
                *out_type = setting->value_type;
            }
        }
        k_mutex_unlock(&settings_lock);
        return ret;
    }
    k_mutex_unlock(&settings_lock);

    struct read_into_context ctx = {.buf = buf, .capacity = capacity, .ret = -EIO};
    int ret = zmk_custom_setting_with_value(setting, read_into_visitor, &ctx);
    if (ret < 0) {
        return ret;
    }
    if (ctx.ret < 0) {
        return ctx.ret;
    }

    if (out_size) {
        *out_size = ctx.out_size;
    }
    if (out_type) {
        *out_type = ctx.out_type;
    }
    return 0;
}

int zmk_custom_setting_value_size(const struct zmk_custom_setting *setting, size_t *out_size) {
    if (!setting || !out_size) {
        return -EINVAL;
    }

    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;
    enum zmk_custom_setting_value_type presented_type =
        keyspace ? keyspace->value_type : setting->value_type;
    if (presented_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
        presented_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (setting_uses_blob_store(setting) && !setting_temporary_active(setting)) {
        size_t blob_size = setting->state->blob.size;
        size_t key_len = keyspace ? keyspace_blob_key_len_locked(setting) : 0;
        *out_size = blob_size > key_len ? blob_size - (key_len ? key_len + 1 : 0) : 0;
        k_mutex_unlock(&settings_lock);
        return 0;
    }
    k_mutex_unlock(&settings_lock);

    struct zmk_custom_setting_value value;
    int ret = keyspace ? keyspace_read_payload(setting, &value)
                       : zmk_custom_setting_read(setting, &value);
    if (ret < 0) {
        /* Shouldn't happen: the large-store branch above already handles any
         * setting whose value could exceed the carrier. */
        return ret;
    }
    *out_size = value.size;
    return 0;
}

int zmk_custom_setting_with_large_raw_bytes(const struct zmk_custom_setting *setting,
                                            zmk_custom_setting_raw_bytes_visitor_t visitor,
                                            void *user_data) {
    if (!setting || !visitor) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (!setting_uses_blob_store(setting) || setting_temporary_active(setting)) {
        k_mutex_unlock(&settings_lock);
        return -ENOTSUP;
    }

    const struct zmk_custom_setting_state *state = setting->state;

    if (setting->_keyspace) {
        /* Stream only the payload slice - skip the embedded
         * "user_key\0" prefix (see the keyspace design comment in the
         * header). Re-derived under the lock every call, same invariant as
         * every other blob.data access in this file. */
        size_t key_len = keyspace_blob_key_len_locked(setting);
        const uint8_t *payload =
            state->blob.size > key_len ? state->blob.data + key_len + 1 : (const uint8_t *)"";
        size_t payload_size = state->blob.size > key_len ? state->blob.size - key_len - 1 : 0;
        visitor(payload, payload_size, user_data);
        k_mutex_unlock(&settings_lock);
        return 0;
    }

    const uint8_t *data = state->blob.size > 0 ? state->blob.data : (const uint8_t *)"";
    visitor(data, state->blob.size, user_data);
    k_mutex_unlock(&settings_lock);
    return 0;
}

/* Apply a large (> carrier) raw BYTES/STRING payload to a blob setting.
 * Caller holds settings_lock. TEMPORARY mode is not supported for
 * large values (the temporary override pool is intentionally small). */
static int write_large_locked(const struct zmk_custom_setting *setting, const void *data,
                              size_t size, enum zmk_custom_setting_write_mode mode) {
    if (size > setting_capacity(setting)) {
        return -EMSGSIZE;
    }

    int ret;
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        ret = blob_store_set_raw(setting, data, size);
        if (ret < 0) {
            return ret;
        }
        set_setting_dirty(setting, true);
        clear_temporary_locked(setting);
        return 0;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        ret = blob_store_set_raw(setting, data, size);
        if (ret < 0) {
            return ret;
        }
        clear_temporary_locked(setting);
        return save_setting_locked(setting);
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY:
        return -EMSGSIZE;
    default:
        return -EINVAL;
    }
}

/*
 * Keyspace-agnostic "write this exact raw payload as the setting's stored
 * value" primitive - today's zmk_custom_setting_write_bytes body prior to
 * simplification P3. Used directly by the public zmk_custom_setting_write_bytes
 * for a normal setting, AND (via its small-carrier tail calling
 * write_scalar_value, not zmk_custom_setting_write) to store an
 * already-assembled keyspace slot blob verbatim - see keyspace_write_blob/
 * keyspace_write_raw_payload further down, which build that blob and call
 * this function directly to avoid re-triggering the keyspace interception in
 * the public dispatchers.
 */
static int write_bytes_raw(const struct zmk_custom_setting *setting, const void *data, size_t size,
                           enum zmk_custom_setting_write_mode mode) {
    /* Blob values exceeding the fixed carrier take a dedicated raw path
     * (the carrier below cannot hold them). Values that still fit the
     * carrier fall through to the normal validated path so constraints keep
     * being enforced. */
    if (setting_uses_blob_store(setting) && size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        if (size > setting_capacity(setting)) {
            return -EMSGSIZE;
        }

        k_mutex_lock(&settings_lock, K_FOREVER);
        int ret = write_large_locked(setting, data, size, mode);
        k_mutex_unlock(&settings_lock);

        if (ret == 0) {
            raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                               ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                               : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
        }
        return ret;
    }

    struct zmk_custom_setting_value value = {.type = setting->value_type};
    switch (setting->value_type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        if (size > sizeof(value.bytes_value)) {
            return -EMSGSIZE;
        }
        value.size = size;
        memcpy(value.bytes_value, data, size);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING: {
        /* Reject rather than silently truncate when the value cannot fit the
         * fixed carrier (mirrors the BYTES guard above). A larger value must
         * be written to a large-capable setting via the raw path. */
        if (size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        memcpy(value.string_value, data, size);
        value.string_value[size] = '\0';
        value.size = size;
        break;
    }
    default:
        return -EINVAL;
    }

    return write_scalar_value(setting, &value, mode);
}

int zmk_custom_setting_write_bytes(const struct zmk_custom_setting *setting, const void *data,
                                   size_t size, enum zmk_custom_setting_write_mode mode) {
    if (!setting || (!data && size > 0)) {
        return -EINVAL;
    }

    if (setting->_keyspace) {
        return keyspace_write_raw_payload(setting, data, size, mode);
    }

    return write_bytes_raw(setting, data, size, mode);
}

int zmk_custom_setting_get_int32(const struct zmk_custom_setting *setting, int32_t *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    enum zmk_custom_setting_value_type type;
    int ret = zmk_custom_setting_read_into(setting, value, sizeof(*value), NULL, &type);
    if (ret < 0) {
        return ret;
    }

    return type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 ? 0 : -EINVAL;
}

int zmk_custom_setting_set_int32(const struct zmk_custom_setting *setting, int32_t value,
                                 enum zmk_custom_setting_write_mode mode) {
    if (!setting) {
        return -EINVAL;
    }

    return zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(value), mode);
}

int zmk_custom_setting_get_bool(const struct zmk_custom_setting *setting, bool *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    enum zmk_custom_setting_value_type type;
    int ret = zmk_custom_setting_read_into(setting, value, sizeof(*value), NULL, &type);
    if (ret < 0) {
        return ret;
    }

    return type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL ? 0 : -EINVAL;
}

int zmk_custom_setting_set_bool(const struct zmk_custom_setting *setting, bool value,
                                enum zmk_custom_setting_write_mode mode) {
    if (!setting) {
        return -EINVAL;
    }

    return zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_BOOL(value), mode);
}

int zmk_custom_setting_get_behavior(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_behavior_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    enum zmk_custom_setting_value_type type;
    int ret = zmk_custom_setting_read_into(setting, value, sizeof(*value), NULL, &type);
    if (ret < 0) {
        return ret;
    }

    return type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR ? 0 : -EINVAL;
}

int zmk_custom_setting_set_behavior(const struct zmk_custom_setting *setting,
                                    struct zmk_custom_setting_behavior_value value,
                                    enum zmk_custom_setting_write_mode mode) {
    if (!setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value setting_value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
        .behavior_value = value,
    };
    return zmk_custom_setting_write(setting, &setting_value, mode);
}

static int value_from_storage(const struct zmk_custom_setting *setting, const void *data,
                              size_t len) {
    /* A blob (BYTES/STRING) payload larger than the fixed carrier loads
     * directly into the blob store, bypassing the carrier (which cannot
     * hold it). Constraints for these types are size-only, already enforced
     * by the capacity check. */
    if (setting_uses_blob_store(setting) && len > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        if (len > setting_capacity(setting)) {
            return -EMSGSIZE;
        }
        int ret = blob_store_set_raw(setting, data, len);
        if (ret < 0) {
            /* Pool exhausted (e.g. the pool shrank across a firmware update,
             * or this boot's other pool members already claim the rest of
             * the budget) - skip this persisted value rather than fail boot,
             * mirroring the keyspace-pool-exhaustion policy. */
            LOG_WRN("Custom settings pool exhausted; skipping persisted value for %s/%s",
                    setting->custom_subsystem_id, setting->key);
            return ret;
        }
        set_setting_has_persistent_value(setting, true);
        set_setting_dirty(setting, false);
        clear_temporary_locked(setting);
        return 0;
    }

    struct zmk_custom_setting_value value = {.type = setting->value_type};

    switch (setting->value_type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        if (len > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        value.size = len;
        memcpy(value.bytes_value, data, len);
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        if (len != sizeof(value.int32_value)) {
            return -EINVAL;
        }
        memcpy(&value.int32_value, data, sizeof(value.int32_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        if (len != sizeof(value.bool_value)) {
            return -EINVAL;
        }
        memcpy(&value.bool_value, data, sizeof(value.bool_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        if (len > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        value.size = len;
        memcpy(value.string_value, data, len);
        value.string_value[len] = '\0';
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR: {
        int ret = decode_behavior_value(data, len, &value.behavior_value);
        if (ret < 0) {
            return ret;
        }
        break;
    }
    default:
        return -EINVAL;
    }

    int ret = zmk_custom_setting_validate(setting, &value);
    if (ret < 0) {
        return ret;
    }

    ret = store_memory_value_locked(setting, &value);
    if (ret < 0) {
        LOG_WRN("Custom settings pool exhausted; skipping persisted value for %s/%s",
                setting->custom_subsystem_id, setting->key);
        return ret;
    }
    set_setting_has_persistent_value(setting, true);
    set_setting_dirty(setting, false);
    clear_temporary_locked(setting);
    return 0;
}

static int array_size_from_storage(const struct zmk_custom_setting *array_element, const void *data,
                                   size_t len) {
    uint32_t array_size;
    if (len != sizeof(array_size)) {
        return -EINVAL;
    }

    memcpy(&array_size, data, sizeof(array_size));
    int ret = validate_array_size(array_element, array_size);
    if (ret < 0) {
        return ret;
    }

    set_array_persistent_size_locked(array_element, array_size);
    set_array_memory_size_locked(array_element, array_size);
    return 0;
}

static bool split_array_size_key(const char *name, char *array_key, size_t array_key_size) {
    size_t name_len = bounded_strlen(name, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN);
    size_t suffix_len = sizeof("/" ARRAY_SIZE_STORAGE_KEY) - 1;

    if (name_len <= suffix_len ||
        strcmp(&name[name_len - suffix_len], "/" ARRAY_SIZE_STORAGE_KEY) != 0) {
        return false;
    }

    size_t key_len = name_len - suffix_len;
    if (key_len == 0 || key_len >= array_key_size) {
        return false;
    }

    memcpy(array_key, name, key_len);
    array_key[key_len] = '\0';
    return true;
}

/* Split a stored per-element name (e.g. "array_value/2") into its array_key
 * ("array_value") and numeric index (2). Array elements are no longer
 * individually registered (see ZMK_CUSTOM_SETTING_ARRAY_DEFINE), so
 * custom_settings_handle_set can no longer resolve them with a plain
 * zmk_custom_setting_find(name) lookup and needs this to route into
 * zmk_custom_setting_find_array_element instead. Storage key format is
 * unchanged from the pre-P3 per-element registrations. */
static bool split_array_element_key(const char *name, char *array_key, size_t array_key_size,
                                    uint32_t *index) {
    size_t name_len = bounded_strlen(name, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN);

    const char *slash = NULL;
    for (size_t i = name_len; i > 0; i--) {
        if (name[i - 1] == '/') {
            slash = &name[i - 1];
            break;
        }
    }
    if (!slash || slash == name) {
        return false;
    }

    const char *index_str = slash + 1;
    size_t index_str_len = &name[name_len] - index_str;
    if (index_str_len == 0) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < index_str_len; i++) {
        if (index_str[i] < '0' || index_str[i] > '9') {
            return false;
        }
        value = value * 10 + (uint32_t)(index_str[i] - '0');
    }

    size_t key_len = (size_t)(slash - name);
    if (key_len == 0 || key_len >= array_key_size) {
        return false;
    }

    memcpy(array_key, name, key_len);
    array_key[key_len] = '\0';
    *index = value;
    return true;
}

/*
 * Simplification P3: opaque-blob keyspaces. See the extended design comment
 * on struct zmk_custom_setting_keyspace in the header for the full picture;
 * this is the implementation of that design's presentation/lookup layer -
 * the ONLY keyspace-aware code in this module (everything else - storage,
 * pool, generic value read/write/save - treats a slot as a bog-standard
 * pooled BYTES setting, per struct zmk_custom_setting's `_keyspace` field
 * doc).
 */

const struct zmk_custom_setting_keyspace *
zmk_custom_setting_keyspace_of(const struct zmk_custom_setting *setting) {
    return setting ? setting->_keyspace : NULL;
}

static bool keyspace_key_matches_prefix(const struct zmk_custom_setting_keyspace *keyspace,
                                        const char *key) {
    size_t prefix_len = strlen(keyspace->key_prefix);
    return strncmp(key, keyspace->key_prefix, prefix_len) == 0;
}

/* Find a keyspace registered for custom_subsystem_id whose key_prefix
 * matches the start of `key`. Caller must hold settings_lock. */
static struct zmk_custom_setting_keyspace *
keyspace_find_for_key_locked(const char *custom_subsystem_id, const char *key) {
    ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(keyspace) {
        if (strncmp(keyspace->custom_subsystem_id, custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
            continue;
        }
        if (keyspace_key_matches_prefix(keyspace, key)) {
            return keyspace;
        }
    }

    return NULL;
}

struct zmk_custom_setting_keyspace *
zmk_custom_settings_keyspace_find_for_key(const char *custom_subsystem_id, const char *key) {
    if (!custom_subsystem_id || !key) {
        return NULL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    struct zmk_custom_setting_keyspace *keyspace =
        keyspace_find_for_key_locked(custom_subsystem_id, key);
    k_mutex_unlock(&settings_lock);

    return keyspace;
}

/* Length of the user-key portion of a keyspace slot's blob (bytes before the
 * embedded NUL separator - see the `[user_key\0][payload]` blob format doc
 * on struct zmk_custom_setting_keyspace in the header), or 0 if the slot has
 * no blob yet (blob.data == NULL - only possible transiently between
 * keyspace_bind_slot_locked binding a slot and the settings-load callback
 * applying its persisted bytes moments later, or between
 * zmk_custom_setting_keyspace_create claiming a slot and writing its first
 * value). Caller holds settings_lock (the blob can move on pool
 * compaction). */
static size_t keyspace_blob_key_len_locked(const struct zmk_custom_setting *setting) {
    const struct zmk_custom_setting_state *state = setting->state;
    if (!state->blob.data || state->blob.size == 0) {
        return 0;
    }
    const uint8_t *nul = memchr(state->blob.data, '\0', state->blob.size);
    return nul ? (size_t)(nul - state->blob.data) : state->blob.size;
}

/* Find the slot index currently bound to `key` (decoding each live slot's
 * blob to compare - the design doc's "find decodes to compare": a short walk
 * over ≤max_entries live slots, not a scan of raw pool bytes). Caller holds
 * settings_lock. */
static int keyspace_slot_index_for_key_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              const char *key) {
    size_t key_len = strlen(key);
    for (uint32_t i = 0; i < keyspace->max_entries; i++) {
        if (!keyspace->slots[i].in_use) {
            continue;
        }
        const struct zmk_custom_setting *slot_setting = &keyspace->slots[i].setting;
        size_t blob_key_len = keyspace_blob_key_len_locked(slot_setting);
        if (blob_key_len == key_len && slot_setting->state->blob.data != NULL &&
            memcmp(slot_setting->state->blob.data, key, key_len) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int keyspace_free_slot_index_locked(struct zmk_custom_setting_keyspace *keyspace) {
    for (uint32_t i = 0; i < keyspace->max_entries; i++) {
        if (!keyspace->slots[i].in_use) {
            return (int)i;
        }
    }

    return -1;
}

/* Bind slot `index` of `keyspace` to its stable ordinal storage identity
 * ("<key_prefix>#<index>") and wire it into the keyspace's shared pool, with
 * an EMPTY blob (blob.data == NULL) - the caller is responsible for
 * populating it immediately after, either via the settings-load value-apply
 * step (a persisted record) or keyspace_write_blob (a fresh create). Caller
 * holds settings_lock. Never fails: the ordinal name buffer is sized by
 * ZMK_CUSTOM_SETTINGS_KEYSPACE_ORDINAL_NAME_SIZE generously enough for any
 * key_prefix + max_entries this module's BUILD_ASSERTs allow. */
static struct zmk_custom_setting *
keyspace_bind_slot_locked(struct zmk_custom_setting_keyspace *keyspace, uint32_t index) {
    struct zmk_custom_setting_keyspace_slot *slot = &keyspace->slots[index];

    snprintf(slot->ordinal_name, sizeof(slot->ordinal_name), "%s#%u", keyspace->key_prefix, index);

    /* Reset the slot's embedded state block first (P4): a freshly bound
     * slot starts with an empty blob (data == NULL - its region is carved
     * from the pool on the first write/load), no flags, no temp slot, and
     * no default override left over from a previous occupant. */
    slot->state = (struct zmk_custom_setting_state){.temp_slot = -1};
    slot->setting = (struct zmk_custom_setting){
        .custom_subsystem_id = keyspace->custom_subsystem_id,
        .key = slot->ordinal_name,
        .array_key = NULL,
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
        .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .confidentiality = keyspace->confidentiality,
        .read_permission = keyspace->read_permission,
        .write_permission = keyspace->write_permission,
        /* Deliberately NOT keyspace->constraints/rpc_*: those describe the
         * PAYLOAD (see zmk_custom_setting_keyspace_of/keyspace_validate_payload),
         * not the slot's own opaque BYTES blob, which has no constraints of
         * its own. */
        .constraints = NULL,
        .constraints_count = 0,
        .default_value = NULL,
        .rpc_serializer = NULL,
        .rpc_deserializer = NULL,
        .blob = {.max_size = keyspace->max_key_len + keyspace->max_size,
                 .pool = keyspace->large_pool},
        ._keyspace = keyspace,
        .state = &slot->state,
    };
    slot->in_use = true;
    init_setting_state_locked(&slot->setting);
    return &slot->setting;
}

/* Release a live slot back to the pool: clears any temporary override,
 * releases its pool region, and marks it free. Caller holds settings_lock. */
static void keyspace_release_slot_locked(struct zmk_custom_setting_keyspace *keyspace,
                                         uint32_t index) {
    struct zmk_custom_setting_keyspace_slot *slot = &keyspace->slots[index];
    clear_temporary_locked(&slot->setting);
    pool_release_locked(&slot->setting);
    slot->in_use = false;
}

/* Validate a PAYLOAD (not a slot's opaque blob) against keyspace->value_type/
 * constraints/max_size - shared by zmk_custom_setting_keyspace_create and
 * zmk_custom_setting_write's keyspace branch. */
static int keyspace_validate_payload(const struct zmk_custom_setting_keyspace *keyspace,
                                     const struct zmk_custom_setting_value *value) {
    struct zmk_custom_setting payload_shape = {
        .value_type = keyspace->value_type,
        .constraints = keyspace->constraints,
        .constraints_count = keyspace->constraints_count,
    };
    int ret = zmk_custom_setting_validate(&payload_shape, value);
    if (ret < 0) {
        return ret;
    }

    if ((value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
         value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) &&
        value->size > keyspace->max_size) {
        /* Per-keyspace value size ceiling, tighter (or looser) than the
         * global CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE - see the field
         * comment on struct zmk_custom_setting_keyspace.max_size. */
        return -EMSGSIZE;
    }

    return 0;
}

/* Scratch buffer for assembling a keyspace slot's `[user_key\0][payload]`
 * blob before handing it to write_bytes_raw. Sized generously off the same
 * two Kconfig bounds ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE's BUILD_ASSERTs tie
 * every keyspace's max_key_len/max_size to, so it covers any registered
 * keyspace regardless of its own (smaller) per-instance limits. Safe as a
 * single shared instance: assembled and consumed synchronously within one
 * keyspace_write_blob/keyspace_write_raw_payload call, matching this file's
 * other scratch-buffer patterns (e.g. behavior_encode_scratch). */
#define ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE                                             \
    (CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN + CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE)
static uint8_t keyspace_blob_scratch[ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE];

/* Shared blob assembly for keyspace_write_blob (typed payload) and
 * keyspace_write_raw_payload (already-raw payload, e.g. the WriteValueChunk
 * commit path or a direct zmk_custom_setting_write_bytes call): builds
 * `blob = [key\0][payload]` and writes it via write_bytes_raw (the
 * keyspace-agnostic pooled-BYTES path) - the only place a keyspace slot's
 * blob bytes are assembled from a (key, payload) pair. `key` is the literal
 * user key for a fresh create, or NULL to reuse the slot's current key (a
 * write to an already-live entry). */
static int keyspace_write_raw_payload_with_key(const struct zmk_custom_setting *setting,
                                               const char *key, const void *payload_data,
                                               size_t payload_len,
                                               enum zmk_custom_setting_write_mode mode) {
    size_t key_len;
    if (key != NULL) {
        key_len = strlen(key);
        if (key_len + 1 > sizeof(keyspace_blob_scratch)) {
            return -ENAMETOOLONG;
        }
        memcpy(keyspace_blob_scratch, key, key_len);
    } else {
        k_mutex_lock(&settings_lock, K_FOREVER);
        key_len = keyspace_blob_key_len_locked(setting);
        key_len = MIN(key_len, sizeof(keyspace_blob_scratch) - 1);
        if (setting->state->blob.data != NULL && key_len > 0) {
            memcpy(keyspace_blob_scratch, setting->state->blob.data, key_len);
        }
        k_mutex_unlock(&settings_lock);
    }
    keyspace_blob_scratch[key_len] = '\0';

    if (key_len + 1 + payload_len > sizeof(keyspace_blob_scratch)) {
        return -EMSGSIZE;
    }
    if (payload_len > 0) {
        memcpy(keyspace_blob_scratch + key_len + 1, payload_data, payload_len);
    }

    return write_bytes_raw(setting, keyspace_blob_scratch, key_len + 1 + payload_len, mode);
}

/* `value` (typed as keyspace->value_type) is converted to raw payload bytes
 * via value_to_storage, then assembled/written via
 * keyspace_write_raw_payload_with_key. Caller has already validated `value`
 * via keyspace_validate_payload. */
static int keyspace_write_blob(const struct zmk_custom_setting *setting, const char *key,
                               const struct zmk_custom_setting_value *value,
                               enum zmk_custom_setting_write_mode mode) {
    const void *payload_data;
    size_t payload_len;
    int ret = value_to_storage(value, &payload_data, &payload_len);
    if (ret < 0) {
        return ret;
    }

    return keyspace_write_raw_payload_with_key(setting, key, payload_data, payload_len, mode);
}

static int keyspace_write_raw_payload(const struct zmk_custom_setting *setting, const void *data,
                                      size_t size, enum zmk_custom_setting_write_mode mode) {
    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;
    if (keyspace->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
        keyspace->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        /* A raw-bytes write only makes sense for a BYTES/STRING-declared
         * keyspace; other payload types must go through
         * zmk_custom_setting_write with a typed value. */
        return -EINVAL;
    }
    if (size > keyspace->max_size) {
        return -EMSGSIZE;
    }

    return keyspace_write_raw_payload_with_key(setting, NULL, data, size, mode);
}

/* Decode a live keyspace slot's blob into its PRESENTED payload value (typed
 * per keyspace->value_type), respecting a temporary override the same way
 * effective_value() already does for any other setting - the full blob
 * (whatever its source: pool region, or a temp_slots entry, which stores the
 * exact bytes written to it, full blob included) is materialized once via
 * effective_value(), then the key prefix is stripped. Returns -EMSGSIZE if
 * the blob itself does not fit the fixed carrier (large payload) - callers
 * then fall back to zmk_custom_setting_with_large_raw_bytes, which is
 * already keyspace-aware. */
static int keyspace_read_payload(const struct zmk_custom_setting *setting,
                                 struct zmk_custom_setting_value *out_value) {
    struct zmk_custom_setting_value blob_value;

    k_mutex_lock(&settings_lock, K_FOREVER);
    const struct zmk_custom_setting_value *effective = effective_value(setting);
    if (!effective) {
        k_mutex_unlock(&settings_lock);
        return -EMSGSIZE;
    }
    copy_value(&blob_value, effective);
    k_mutex_unlock(&settings_lock);

    const uint8_t *nul = memchr(blob_value.bytes_value, '\0', blob_value.size);
    size_t key_len = nul ? (size_t)(nul - blob_value.bytes_value) : blob_value.size;
    const uint8_t *payload =
        blob_value.size > key_len ? &blob_value.bytes_value[key_len + 1] : blob_value.bytes_value;
    size_t payload_len = blob_value.size > key_len ? blob_value.size - key_len - 1 : 0;

    value_from_raw(out_value, setting->_keyspace->value_type, payload, payload_len);
    return 0;
}

/* Keyspace counterpart of zmk_custom_setting_read_into: strips the key
 * prefix from either the raw large-store payload (any size) or the
 * carrier-sized/temporary-override path (via keyspace_read_payload +
 * read_into_visitor, reusing the exact same raw-conversion logic a normal
 * setting's read_into uses). */
static int keyspace_read_into(const struct zmk_custom_setting *setting, void *buf, size_t capacity,
                              size_t *out_size, enum zmk_custom_setting_value_type *out_type) {
    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (setting_uses_blob_store(setting) && !setting_temporary_active(setting) &&
        setting->state->blob.size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        const struct zmk_custom_setting_state *state = setting->state;
        size_t key_len = keyspace_blob_key_len_locked(setting);
        size_t payload_size = state->blob.size > key_len ? state->blob.size - key_len - 1 : 0;
        int ret = 0;
        if (payload_size > capacity) {
            ret = -EMSGSIZE;
        } else {
            if (payload_size > 0) {
                memcpy(buf, state->blob.data + key_len + 1, payload_size);
            }
            if (out_size) {
                *out_size = payload_size;
            }
            if (out_type) {
                *out_type = keyspace->value_type;
            }
        }
        k_mutex_unlock(&settings_lock);
        return ret;
    }
    k_mutex_unlock(&settings_lock);

    struct zmk_custom_setting_value value;
    int ret = keyspace_read_payload(setting, &value);
    if (ret < 0) {
        return ret;
    }

    struct read_into_context ctx = {.buf = buf, .capacity = capacity, .ret = -EIO};
    read_into_visitor(&value, &ctx);
    if (ctx.ret < 0) {
        return ctx.ret;
    }

    if (out_size) {
        *out_size = ctx.out_size;
    }
    if (out_type) {
        *out_type = ctx.out_type;
    }
    return 0;
}

const struct zmk_custom_setting *
zmk_custom_setting_keyspace_find(const struct zmk_custom_setting_keyspace *keyspace,
                                 const char *key) {
    if (!keyspace || !key) {
        return NULL;
    }

    struct zmk_custom_setting_keyspace *mutable_keyspace =
        (struct zmk_custom_setting_keyspace *)keyspace;

    k_mutex_lock(&settings_lock, K_FOREVER);
    int index = keyspace_slot_index_for_key_locked(mutable_keyspace, key);
    k_mutex_unlock(&settings_lock);

    return index >= 0 ? &keyspace->slots[index].setting : NULL;
}

int zmk_custom_setting_keyspace_create(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key,
                                       const struct zmk_custom_setting_value *value,
                                       enum zmk_custom_setting_write_mode mode,
                                       const struct zmk_custom_setting **out_setting) {
    if (!keyspace || !key || !value) {
        return -EINVAL;
    }

    if (!keyspace_key_matches_prefix(keyspace, key)) {
        return -EINVAL;
    }

    int ret = keyspace_validate_payload(keyspace, value);
    if (ret < 0) {
        return ret;
    }

    size_t key_len = bounded_strlen(key, keyspace->max_key_len);
    if (key_len == 0 || key_len >= keyspace->max_key_len || key[key_len] != '\0') {
        return -ENAMETOOLONG;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);

    if (keyspace_slot_index_for_key_locked(keyspace, key) >= 0) {
        k_mutex_unlock(&settings_lock);
        return -EEXIST;
    }

    int index = keyspace_free_slot_index_locked(keyspace);
    if (index < 0) {
        k_mutex_unlock(&settings_lock);
        return -ENOSPC;
    }

    struct zmk_custom_setting *setting = keyspace_bind_slot_locked(keyspace, (uint32_t)index);
    k_mutex_unlock(&settings_lock);

    ret = keyspace_write_blob(setting, key, value, mode);
    if (ret < 0) {
        /* Roll back: writing the initial value failed (e.g. constraint
         * violation at write time, or -ENOSPC from the pool), so do not
         * leave a half-created entry occupying a slot. */
        k_mutex_lock(&settings_lock, K_FOREVER);
        keyspace_release_slot_locked(keyspace, (uint32_t)index);
        k_mutex_unlock(&settings_lock);
        return ret;
    }

    if (out_setting) {
        *out_setting = setting;
    }
    return 0;
}

int zmk_custom_setting_keyspace_delete(struct zmk_custom_setting_keyspace *keyspace,
                                       const char *key) {
    if (!keyspace || !key) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    int index = keyspace_slot_index_for_key_locked(keyspace, key);
    if (index < 0) {
        k_mutex_unlock(&settings_lock);
        return -ENOENT;
    }
    struct zmk_custom_setting *setting = &keyspace->slots[index].setting;
    k_mutex_unlock(&settings_lock);

    /* Erase the persisted record (if any) the same way a scalar setting's
     * reset does, then release the slot. Ignore -ENOENT from a setting that
     * was never saved. */
    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(setting, name, sizeof(name));
    if (ret == 0) {
        ret = settings_delete(name);
        if (ret == -ENOENT) {
            ret = 0;
        }
    }
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    keyspace_release_slot_locked(keyspace, (uint32_t)index);
    k_mutex_unlock(&settings_lock);

    return 0;
}

/* Parse a stored record name's remainder (after the custom_subsystem_id
 * component split off by settings_name_next, e.g. "macro/#3" for a keyspace
 * whose key_prefix is "macro/") as "<key_prefix>#<index>" for `keyspace`.
 * Zephyr's settings subsystem only treats '/' as a hierarchy separator -
 * '#' is an ordinary name byte, so this format needs no escaping. */
static bool keyspace_parse_ordinal_name(const struct zmk_custom_setting_keyspace *keyspace,
                                        const char *name, uint32_t *out_index) {
    size_t prefix_len = strlen(keyspace->key_prefix);
    if (strncmp(name, keyspace->key_prefix, prefix_len) != 0 || name[prefix_len] != '#') {
        return false;
    }

    const char *digits = name + prefix_len + 1;
    if (*digits == '\0') {
        return false;
    }

    uint32_t value = 0;
    for (const char *p = digits; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + (uint32_t)(*p - '0');
    }

    *out_index = value;
    return true;
}

static int custom_settings_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                      void *cb_arg) {
    const char *next;
    char custom_subsystem_id[CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN];

    int match_len = settings_name_next(name, &next);
    if (match_len <= 0 || match_len >= sizeof(custom_subsystem_id) || !next) {
        return -ENOENT;
    }

    memcpy(custom_subsystem_id, name, match_len);
    custom_subsystem_id[match_len] = '\0';

    char array_key[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    if (split_array_size_key(next, array_key, sizeof(array_key))) {
        struct zmk_custom_setting *setting =
            (struct zmk_custom_setting *)zmk_custom_setting_find_array_element(custom_subsystem_id,
                                                                               array_key, 0);
        if (!setting) {
            return -ENOENT;
        }

        uint32_t array_size;
        if (len != sizeof(array_size)) {
            return -EINVAL;
        }

        ssize_t read = read_cb(cb_arg, &array_size, sizeof(array_size));
        if (read < 0) {
            return read;
        }

        k_mutex_lock(&settings_lock, K_FOREVER);
        int ret = array_size_from_storage(setting, &array_size, read);
        k_mutex_unlock(&settings_lock);
        return ret;
    }

    struct zmk_custom_setting *setting;
    uint32_t element_index;
    if (split_array_element_key(next, array_key, sizeof(array_key), &element_index)) {
        /* Array elements are no longer individually registered (see
         * ZMK_CUSTOM_SETTING_ARRAY_DEFINE), so a stored "array_key/index"
         * name is resolved through the index-view mechanism instead of a
         * direct registry lookup by key. */
        setting = (struct zmk_custom_setting *)zmk_custom_setting_find_array_element(
            custom_subsystem_id, array_key, element_index);
    } else {
        setting = (struct zmk_custom_setting *)zmk_custom_setting_find(custom_subsystem_id, next);
        if (!setting) {
            /* Simplification P3: `next` may be a keyspace slot's stable
             * ordinal storage name, "<key_prefix>#<index>" (e.g. "macro/#3"),
             * for a slot that has not been (re)bound yet this boot - e.g. a
             * fresh boot loading a value persisted by a CreateSetting RPC in
             * a previous session, with no application code creating the
             * entry again. Bind slot `index` directly here (no key needed
             * yet - it lives inside the blob about to be loaded) and fall
             * through to apply this record's value to the freshly bound
             * descriptor exactly like any other setting - this is the
             * "re-bound during settings load" requirement: no module code
             * has to run for user-created entries to survive reboot. */
            k_mutex_lock(&settings_lock, K_FOREVER);
            ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(keyspace) {
                if (strncmp(keyspace->custom_subsystem_id, custom_subsystem_id,
                            CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
                    continue;
                }
                uint32_t index;
                if (!keyspace_parse_ordinal_name(keyspace, next, &index)) {
                    continue;
                }
                if (index >= keyspace->max_entries) {
                    LOG_WRN("Custom settings keyspace record %s/%s has out-of-range ordinal index "
                            "%u (max_entries=%u) - dropping",
                            custom_subsystem_id, next, index, keyspace->max_entries);
                    continue;
                }
                if (keyspace->slots[index].in_use) {
                    /* Already bound this boot (e.g. a targeted
                     * settings_load_subtree from zmk_custom_setting_discard,
                     * or a repeated settings_load pass): apply the record to
                     * the LIVE slot rather than re-binding it -
                     * keyspace_bind_slot_locked resets the descriptor
                     * (blob.data/_pool_next), which would corrupt the
                     * pool's member list for a slot that already holds a
                     * region. */
                    setting = &keyspace->slots[index].setting;
                } else {
                    setting = keyspace_bind_slot_locked(keyspace, index);
                }
                break;
            }
            k_mutex_unlock(&settings_lock);
        }
    }
    if (!setting) {
        return -ENOENT;
    }

    /* Sized to the largest per-setting value so a large-store setting's
     * persisted payload can be staged on load, not just the fixed carrier -
     * a keyspace slot's blob additionally carries its user key alongside
     * the payload, so this is wider than CONFIG_ZMK_CUSTOM_SETTINGS_
     * LARGE_VALUE_MAX_SIZE alone to fit that too (see
     * ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE, which every
     * registered keyspace's own max_key_len/max_size are bounded against). */
    uint8_t data[ZMK_CUSTOM_SETTINGS_KEYSPACE_BLOB_SCRATCH_SIZE];
    if (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        len = sizeof(int32_t);
    } else if (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        len = sizeof(bool);
    }

    if (len > sizeof(data)) {
        return -EMSGSIZE;
    }

    ssize_t read = read_cb(cb_arg, data, len);
    if (read < 0) {
        return read;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    int ret = value_from_storage(setting, data, read);
    k_mutex_unlock(&settings_lock);
    return ret;
}

SETTINGS_STATIC_HANDLER_DEFINE(custom_settings, SETTINGS_SUBTREE, NULL, custom_settings_handle_set,
                               NULL, NULL);

/* Reset one setting's mutable state to its just-registered, nothing-loaded-
 * yet state: defaults copied in, no persistent/dirty/temporary flags set.
 * Shared by custom_settings_init() (every compile-time setting, at boot)
 * and keyspace_bind_slot_locked() (one keyspace slot, (re)bound at any
 * point - boot-time settings-load or a later CreateSetting RPC) so the two
 * initialization paths cannot drift apart. Caller must hold settings_lock if
 * called after boot (custom_settings_init runs before any other thread can
 * contend for it, so it does not bother). */
static void init_setting_state_locked(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting)) {
        /* One call covers the whole array (there is exactly one registered
         * descriptor per array - see ZMK_CUSTOM_SETTING_ARRAY_DEFINE):
         * initialize every element slot from its own per-index default
         * instead of walking "sibling" registrations. */
        struct zmk_custom_setting_array_state *array_state = setting->array_state;
        for (uint32_t index = 0; index < array_state->max_size; index++) {
            copy_value(&array_state->values[index], &array_state->defaults[index]);
            array_state->has_persistent[index] = false;
            array_state->dirty[index] = false;
        }
        array_state->persistent_size = array_state->default_size;
        array_state->size = array_state->default_size;
    } else {
        /* P4 note: this deliberately does NOT clear
         * state->default_override or state->blob.data.
         * - default_override: zmk_custom_setting_set_default() may legally
         *   run from a SYS_INIT ordered BEFORE this module's own init;
         *   apply_scalar_default_locked() below reads through
         *   setting_default_value(), so an already-installed override is
         *   both preserved and applied. (Keyspace binds reset the whole
         *   embedded state block beforehand - see
         *   keyspace_bind_slot_locked.)
         * - blob.data: for a fixed-store setting it points permanently at
         *   the macro-emitted buffer. */
        apply_scalar_default_locked(setting);
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT, false);
        state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_DIRTY, false);
    }
    setting->state->temp_slot = -1;
    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_TEMPORARY_ACTIVE, false);
    state_flag_set(setting, ZMK_CUSTOM_SETTING_STATE_INITIALIZED, true);
}

static int custom_settings_init(void) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) { init_setting_state_locked(setting); }

    return 0;
}

SYS_INIT(custom_settings_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
