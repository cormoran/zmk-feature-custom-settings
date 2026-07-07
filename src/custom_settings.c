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

/* Forward declaration: defined alongside zmk_custom_settings_register()
 * (P5a) further down, but also needed by the P5b keyspace bind path
 * (zmk_custom_setting_keyspace_bind_locked, defined earlier in this file,
 * runs from inside the SETTINGS_STATIC_HANDLER_DEFINE callback). See its
 * definition for why the settings_load_subtree() call in
 * zmk_custom_settings_register() itself is split out into this helper. */
static void register_runtime_locked(struct zmk_custom_setting *desc);

/* Forward declaration: array_view_acquire() (defined below) recycles pooled
 * views and must release any temporary override the evicted view still owns,
 * but clear_temporary_locked() is defined further down alongside the rest of
 * the temp-slot helpers. */
static void clear_temporary_locked(struct zmk_custom_setting *setting);

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
 * Per-setting large-value backing store (issue #16).
 *
 * A BYTES/STRING setting registered against a large-value pool
 * (ZMK_CUSTOM_SETTING_DEFINE_POOLED, or ZMK_CUSTOM_SETTING_DEFINE_SIZED's
 * private single-member pool) keeps its payload in a region carved from
 * `large_pool` instead of the fixed-size `memory_value` union, so one large
 * setting does not inflate every other setting. `large_data` is NULL until
 * the first non-empty write - see pool_ensure_region() below - and is
 * (re)pointed at a pool region on demand. These helpers centralize deciding
 * whether a setting uses that store and reading/writing it, so the rest of
 * the file only needs a handful of `setting_uses_large_store()` branches at
 * the value-access sites.
 */
static size_t setting_value_capacity(const struct zmk_custom_setting *setting) {
    return setting->value_max_size ? setting->value_max_size
                                   : CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE;
}

static bool setting_uses_large_store(const struct zmk_custom_setting *setting) {
    return setting->large_pool != NULL &&
           (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
            setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING);
}

/* Bytes a pool member currently occupies: its payload plus the trailing NUL
 * for STRING (large_store_set_raw always writes that NUL inside the region,
 * so it must count toward the region's extent). 0 for a member with no
 * region (large_data == NULL). */
static size_t pool_member_extent(const struct zmk_custom_setting *setting) {
    if (setting->large_data == NULL) {
        return 0;
    }
    return setting->large_size +
           (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING ? 1 : 0);
}

/* Link `setting` into `pool`'s intrusive member list. Caller holds
 * settings_lock and must only call this the moment `setting->large_data`
 * transitions from NULL to non-NULL (i.e. `setting` is not already a
 * member). */
static void pool_link_locked(struct zmk_custom_setting_large_pool *pool,
                             struct zmk_custom_setting *setting) {
    setting->_pool_next = pool->members;
    pool->members = setting;
}

/* Unlink `setting` from `pool`'s intrusive member list. Caller holds
 * settings_lock. A no-op if `setting` is not currently a member (e.g. it
 * never allocated a region). */
static void pool_unlink_locked(struct zmk_custom_setting_large_pool *pool,
                               struct zmk_custom_setting *setting) {
    if (pool->members == setting) {
        pool->members = setting->_pool_next;
        setting->_pool_next = NULL;
        return;
    }

    struct zmk_custom_setting *prev = pool->members;
    while (prev && prev->_pool_next != setting) {
        prev = prev->_pool_next;
    }
    if (prev) {
        prev->_pool_next = setting->_pool_next;
        setting->_pool_next = NULL;
    }
}

/*
 * Release `setting`'s pool region (if any) and unlink it from its pool's
 * member list. Caller holds settings_lock. Must run before a runtime-
 * registered setting's storage is detached/reused (zmk_custom_settings_
 * unregister) - otherwise the pool's member list would keep a dangling
 * pointer at storage that no longer represents this setting (see the
 * intrusive-list note on struct zmk_custom_setting_large_pool). A no-op for
 * a non-pooled setting or one with no allocated region.
 */
static void pool_release_locked(struct zmk_custom_setting *setting) {
    if (setting->large_pool == NULL || setting->large_data == NULL) {
        return;
    }
    pool_unlink_locked(setting->large_pool, setting);
    setting->large_data = NULL;
    setting->large_size = 0;
}

/*
 * Ensure a pooled setting (setting->large_pool != NULL) has a region of at
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
 * owning setting's current large_size, so "compacting" just means walking
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
 * construction one with large_data != NULL, since that is the pool's
 * membership invariant - see pool_link_locked/pool_unlink_locked.
 */
static int pool_ensure_region(struct zmk_custom_setting *setting, size_t needed) {
    struct zmk_custom_setting_large_pool *pool = setting->large_pool;

    if (needed == 0) {
        if (setting->large_data != NULL) {
            pool_unlink_locked(pool, setting);
            setting->large_data = NULL;
        }
        return 0;
    }

    if (setting->large_data != NULL && needed <= pool_member_extent(setting)) {
        /* Shrinking (or an unchanged size) keeps writing into the same
         * region - no need to move anything. */
        return 0;
    }

    /* Feasibility check before moving anything: every other member of this
     * pool keeps its current extent, plus the room this write needs. */
    size_t other_total = 0;
    for (struct zmk_custom_setting *other = pool->members; other; other = other->_pool_next) {
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
        struct zmk_custom_setting *next = NULL;
        for (struct zmk_custom_setting *other = pool->members; other; other = other->_pool_next) {
            if (other == setting) {
                continue;
            }
            if (have_last_addr && other->large_data <= last_addr) {
                /* Already compacted in an earlier iteration. */
                continue;
            }
            if (!next || other->large_data < next->large_data) {
                next = other;
            }
        }
        if (!next) {
            break;
        }

        have_last_addr = true;
        last_addr = next->large_data;

        size_t extent = pool_member_extent(next);
        if (next->large_data != cursor) {
            memmove(cursor, next->large_data, extent);
            next->large_data = cursor;
        }
        cursor += extent;
    }

    bool was_member = setting->large_data != NULL;
    setting->large_data = cursor;
    if (!was_member) {
        pool_link_locked(pool, setting);
    }
    return 0;
}

/* Store `size` raw payload bytes into a large-store setting's buffer. Caller
 * must have validated size <= setting_value_capacity(setting). For a pooled
 * setting, first (re)points large_data at a big-enough region via
 * pool_ensure_region(), possibly relocating other members of the same pool;
 * on -ENOSPC nothing is modified. */
static int large_store_set_raw(struct zmk_custom_setting *setting, const void *data, size_t size) {
    if (setting->large_pool != NULL) {
        size_t needed =
            size + (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING ? 1 : 0);
        int ret = pool_ensure_region(setting, needed);
        if (ret < 0) {
            return ret;
        }
    }

    if (size > 0) {
        memcpy(setting->large_data, data, size);
    }
    if (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING &&
        setting->large_data != NULL) {
        setting->large_data[size] = '\0';
    }
    setting->large_size = size;
    return 0;
}

/* Copy a small carrier value (BYTES/STRING, <= carrier size) into a
 * large-store setting's buffer - used when a large-capable setting is written
 * with a value that happens to fit the fixed carrier (the normal
 * zmk_custom_setting_write / default-application path). Returns -ENOSPC for
 * a pooled setting whose backing pool has no room (see large_store_set_raw);
 * the setting's previous value is left untouched in that case. */
static int large_store_set_value(struct zmk_custom_setting *setting,
                                 const struct zmk_custom_setting_value *value) {
    if (value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        size_t len = bounded_strlen(value->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        len = MIN(len, setting_value_capacity(setting));
        return large_store_set_raw(setting, value->string_value, len);
    } else {
        size_t len = MIN(value->size, setting_value_capacity(setting));
        return large_store_set_raw(setting, value->bytes_value, len);
    }
}

/* Apply a scalar setting's compile-time default to its memory storage,
 * routing large-store settings to their buffer and everything else to the
 * union. Caller holds settings_lock. An empty (size 0) default never
 * allocates a pool region (large_store_set_raw's needed == 0 case). A
 * pooled setting whose non-empty default cannot fit its pool at init time
 * logs and leaves the setting without a region rather than failing boot. */
static void apply_scalar_default_locked(struct zmk_custom_setting *setting) {
    if (setting_uses_large_store(setting)) {
        int ret = large_store_set_value(setting, setting->default_value);
        if (ret < 0) {
            LOG_ERR("Custom settings pool has no room for %s/%s's default value (%d)",
                    setting->custom_subsystem_id, setting->key, ret);
        }
    } else {
        copy_value(&setting->memory_value, setting->default_value);
    }
}

size_t zmk_custom_setting_large_pool_used(const struct zmk_custom_setting_large_pool *pool) {
    if (!pool) {
        return 0;
    }

    size_t used = 0;
    k_mutex_lock(&settings_lock, K_FOREVER);
    for (struct zmk_custom_setting *member = pool->members; member; member = member->_pool_next) {
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
    free_slot->view.has_persistent_value = false;
    free_slot->view.dirty = false;
    free_slot->view.temporary_active = false;
    free_slot->view.temp_slot = -1;
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
 * Use this instead of assigning setting->temporary_active = false directly
 * so the pool slot is not leaked. */
static void clear_temporary_locked(struct zmk_custom_setting *setting) {
    /* Check slot ownership (not just setting->temp_slot >= 0) so a setting
     * that never initialized temp_slot to -1 (e.g. a hand-built
     * STRUCT_SECTION_ITERABLE instance that skips the registration macros,
     * where an unset int8_t field zero-initializes to slot 0) cannot free a
     * slot actually owned by a different setting. */
    if (setting->temp_slot >= 0 && (size_t)setting->temp_slot < ARRAY_SIZE(temp_slots) &&
        temp_slots[setting->temp_slot].owner == setting) {
        temp_slot_free(setting->temp_slot);
    }
    setting->temp_slot = -1;
    setting->temporary_active = false;
}

/*
 * Both scalar settings and array element views ultimately need a mutable
 * struct zmk_custom_setting_value* to read/write the "memory" value from.
 * Scalars store it directly (setting->memory_value); array element views
 * redirect into the owning descriptor's shared contiguous buffer at
 * setting->array_index. Centralizing this here means effective_value(),
 * write_value_locked(), zmk_custom_setting_has_unsaved_value(), etc. do not
 * need their own is_array() branches to find the right storage.
 */
static struct zmk_custom_setting_value *
memory_value_slot(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        return &setting->array_state->values[setting->array_index];
    }

    return (struct zmk_custom_setting_value *)&setting->memory_value;
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

    return setting->dirty;
}

static void set_setting_dirty(const struct zmk_custom_setting *setting, bool dirty) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        *array_dirty_slot(setting) = dirty;
        return;
    }

    ((struct zmk_custom_setting *)setting)->dirty = dirty;
}

static bool setting_has_persistent_value(const struct zmk_custom_setting *setting) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        return *array_has_persistent_slot(setting);
    }

    return setting->has_persistent_value;
}

static void set_setting_has_persistent_value(const struct zmk_custom_setting *setting, bool value) {
    if (zmk_custom_setting_is_array(setting) &&
        setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        *array_has_persistent_slot(setting) = value;
        return;
    }

    ((struct zmk_custom_setting *)setting)->has_persistent_value = value;
}

/*
 * Return the effective value as a fixed carrier, or NULL when the value is a
 * large-store payload that does not fit the carrier (callers must then use
 * zmk_custom_setting_read_into / the chunked RPC instead). Temporary
 * overrides and array elements are always carrier-sized; a large-store scalar
 * currently holding a small value is materialized into effective_scratch_value.
 */
static const struct zmk_custom_setting_value *
effective_value(const struct zmk_custom_setting *setting) {
    if (setting->temporary_active && setting->temp_slot >= 0) {
        const struct zmk_custom_settings_temp_slot *slot = &temp_slots[setting->temp_slot];
        value_from_raw(&temp_scratch_value, slot->type, slot->data, slot->size);
        return &temp_scratch_value;
    }

    if (setting_uses_large_store(setting)) {
        if (setting->large_size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return NULL;
        }
        /* A pooled setting with no region yet (large_data == NULL) holds an
         * empty value - value_from_raw must not be handed a NULL source
         * pointer even for a zero-length copy. */
        value_from_raw(&effective_scratch_value, setting->value_type,
                       setting->large_data != NULL ? setting->large_data : "", setting->large_size);
        return &effective_scratch_value;
    }

    return memory_value_slot(setting);
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

    return NULL;
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

const char *zmk_custom_setting_public_key(const struct zmk_custom_setting *setting) {
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

int zmk_custom_setting_set_default(const struct zmk_custom_setting *const_setting,
                                   const struct zmk_custom_setting_value *value) {
    if (!const_setting || !value) {
        return -EINVAL;
    }

    /* Array elements have per-index defaults from array_state->defaults,
     * set once at registration time (ZMK_CUSTOM_SETTING_ARRAY_DEFINE); there
     * is no runtime-replaceable default_value for an array element/view. */
    if (zmk_custom_setting_is_array(const_setting)) {
        return -ENOTSUP;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    int ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    setting->default_value = value;
    /* No persisted value has been loaded and no in-memory write has happened
     * yet, so the current memory_value (whatever it was materialized to, or
     * even its zero-initialized pre-init state) still represents "unset" -
     * refresh it too. This makes the call safe regardless of whether it runs
     * before or after this module's own registry init, as long as it is
     * before settings_load() (i.e. from any SYS_INIT). */
    if (!setting->has_persistent_value && !setting->dirty) {
        if (setting_uses_large_store(setting)) {
            int large_ret = large_store_set_value(setting, value);
            if (large_ret < 0) {
                k_mutex_unlock(&settings_lock);
                return large_ret;
            }
        } else {
            copy_value(&setting->memory_value, value);
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

static int save_setting_locked(struct zmk_custom_setting *setting) {
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
    if (setting_uses_large_store(setting)) {
        /* Large BYTES/STRING payload is stored raw; persist it directly
         * (bypassing the fixed carrier value_to_storage would use). A pooled
         * setting with no region (empty value) has large_data == NULL -
         * settings_save_one(name, NULL, 0) means "delete this record" to the
         * settings subsystem, not "save an empty value", so substitute a
         * valid (never dereferenced past its 0-byte length) pointer. */
        data = setting->large_data != NULL ? setting->large_data : "";
        len = setting->large_size;
    } else {
        ret = value_to_storage(memory_value_slot(setting), &data, &len);
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

    if (setting->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
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
    return convert_rpc_bytes_value(setting, internal_value, rpc_value,
                                   setting ? setting->rpc_serializer : NULL);
}

int zmk_custom_setting_deserialize_rpc_value(const struct zmk_custom_setting *setting,
                                             const struct zmk_custom_setting_value *rpc_value,
                                             struct zmk_custom_setting_value *internal_value) {
    return convert_rpc_bytes_value(setting, rpc_value, internal_value,
                                   setting ? setting->rpc_deserializer : NULL);
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

/* Apply a write in the selected mode to an already-resolved, in-range
 * setting. Caller must hold settings_lock. */
static int write_value_locked(struct zmk_custom_setting *setting,
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
        setting->temp_slot = slot;
        setting->temporary_active = true;
        return 0;
    }
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        if (setting_uses_large_store(setting)) {
            int ret = large_store_set_value(setting, value);
            if (ret < 0) {
                return ret;
            }
        } else {
            copy_value(memory_value_slot(setting), value);
        }
        set_setting_dirty(setting, true);
        clear_temporary_locked(setting);
        return 0;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        if (setting_uses_large_store(setting)) {
            int ret = large_store_set_value(setting, value);
            if (ret < 0) {
                return ret;
            }
        } else {
            copy_value(memory_value_slot(setting), value);
        }
        clear_temporary_locked(setting);
        return save_setting_locked(setting);
    default:
        return -EINVAL;
    }
}

int zmk_custom_setting_write(const struct zmk_custom_setting *const_setting,
                             const struct zmk_custom_setting_value *value,
                             enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !value) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;
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

    if (setting->has_persistent_value) {
        /* No RAM-resident persistent_value copy is kept; re-read the
         * persisted value from flash straight into memory_value. Discard is
         * a rare, explicit user action, so a flash read here is fine. */
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
    setting->dirty = false;
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
        setting->has_persistent_value = false;
        setting->dirty = false;
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
    bool has_unsaved = setting->temporary_active || setting_is_dirty(setting);
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

    /* Large-store fast path: read the raw payload straight from the setting's
     * buffer, so a value larger than the fixed carrier is still readable (the
     * effective_value carrier below cannot represent it). Skipped while a
     * temporary override is active - those are always carrier-sized. */
    k_mutex_lock(&settings_lock, K_FOREVER);
    if (setting_uses_large_store(setting) && !setting->temporary_active) {
        size_t size = setting->large_size;
        int ret = 0;
        if (size > capacity) {
            ret = -EMSGSIZE;
        } else {
            /* A pooled setting with no region yet (large_data == NULL) has
             * size == 0 here; skip the copy rather than pass a NULL source
             * to memcpy. */
            if (size > 0) {
                memcpy(buf, setting->large_data, size);
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
    if (setting->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
        setting->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (setting_uses_large_store(setting) && !setting->temporary_active) {
        *out_size = setting->large_size;
        k_mutex_unlock(&settings_lock);
        return 0;
    }

    const struct zmk_custom_setting_value *effective = effective_value(setting);
    if (!effective) {
        /* Shouldn't happen: the large-store branch above already handles any
         * setting whose value could exceed the carrier. */
        k_mutex_unlock(&settings_lock);
        return -EMSGSIZE;
    }
    *out_size = effective->size;
    k_mutex_unlock(&settings_lock);
    return 0;
}

int zmk_custom_setting_with_large_raw_bytes(const struct zmk_custom_setting *setting,
                                            zmk_custom_setting_raw_bytes_visitor_t visitor,
                                            void *user_data) {
    if (!setting || !visitor) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (!setting_uses_large_store(setting) || setting->temporary_active) {
        k_mutex_unlock(&settings_lock);
        return -ENOTSUP;
    }

    const uint8_t *data = setting->large_size > 0 ? setting->large_data : (const uint8_t *)"";
    visitor(data, setting->large_size, user_data);
    k_mutex_unlock(&settings_lock);
    return 0;
}

/* Apply a large (> carrier) raw BYTES/STRING payload to a large-store
 * setting. Caller holds settings_lock. TEMPORARY mode is not supported for
 * large values (the temporary override pool is intentionally small). */
static int write_large_locked(struct zmk_custom_setting *setting, const void *data, size_t size,
                              enum zmk_custom_setting_write_mode mode) {
    if (size > setting_value_capacity(setting)) {
        return -EMSGSIZE;
    }

    int ret;
    switch (mode) {
    case ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY:
        ret = large_store_set_raw(setting, data, size);
        if (ret < 0) {
            return ret;
        }
        set_setting_dirty(setting, true);
        clear_temporary_locked(setting);
        return 0;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        ret = large_store_set_raw(setting, data, size);
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

int zmk_custom_setting_write_bytes(const struct zmk_custom_setting *const_setting, const void *data,
                                   size_t size, enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || (!data && size > 0)) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    /* Large-store settings whose value exceeds the fixed carrier take a
     * dedicated raw path (the carrier below cannot hold it). Values that
     * still fit the carrier fall through to the normal validated path so
     * constraints keep being enforced. */
    if (setting_uses_large_store(setting) && size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        if (setting->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
            setting->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
            return -EINVAL;
        }
        if (size > setting_value_capacity(setting)) {
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

    return zmk_custom_setting_write(setting, &value, mode);
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

static int value_from_storage(struct zmk_custom_setting *setting, const void *data, size_t len) {
    /* Large-store BYTES/STRING settings load their raw payload directly into
     * their per-setting buffer, bypassing the fixed carrier (which cannot
     * hold it). Constraints for these types are size-only, already enforced
     * by the capacity check. */
    if (setting_uses_large_store(setting) && len > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        if (len > setting_value_capacity(setting)) {
            return -EMSGSIZE;
        }
        int ret = large_store_set_raw(setting, data, len);
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

    if (setting_uses_large_store(setting)) {
        int large_ret = large_store_set_value(setting, &value);
        if (large_ret < 0) {
            LOG_WRN("Custom settings pool exhausted; skipping persisted value for %s/%s",
                    setting->custom_subsystem_id, setting->key);
            return large_ret;
        }
    } else {
        copy_value(memory_value_slot(setting), &value);
    }
    set_setting_has_persistent_value(setting, true);
    set_setting_dirty(setting, false);
    clear_temporary_locked(setting);
    return 0;
}

static int array_size_from_storage(struct zmk_custom_setting *array_element, const void *data,
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
 * P5b: RPC-creatable keyspaces (ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE). See the
 * extended design comment in the header. Keyspaces are tracked in their own
 * runtime singly-linked list (mirroring runtime_settings_head/_runtime_next
 * for plain runtime settings), separate from the setting registry itself:
 * a keyspace is metadata describing a slot pool, not a setting.
 */

static struct zmk_custom_setting_keyspace *runtime_keyspaces_head;

struct zmk_custom_setting_keyspace *zmk_custom_settings_keyspace_foreach_first(void) {
    return runtime_keyspaces_head;
}

struct zmk_custom_setting_keyspace *
zmk_custom_settings_keyspace_foreach_next(struct zmk_custom_setting_keyspace *current) {
    return current ? current->_next : NULL;
}

int zmk_custom_settings_register_keyspace(struct zmk_custom_setting_keyspace *keyspace) {
    if (!keyspace || !keyspace->custom_subsystem_id || !keyspace->key_prefix ||
        keyspace->key_prefix[0] == '\0' || !keyspace->slots || !keyspace->keys ||
        keyspace->max_entries == 0 || keyspace->max_key_len == 0) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);

    ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(existing) {
        if (existing == keyspace) {
            k_mutex_unlock(&settings_lock);
            return -EEXIST;
        }
        if (strncmp(existing->custom_subsystem_id, keyspace->custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) == 0 &&
            strcmp(existing->key_prefix, keyspace->key_prefix) == 0) {
            k_mutex_unlock(&settings_lock);
            return -EEXIST;
        }
    }

    for (uint32_t i = 0; i < keyspace->max_entries; i++) {
        keyspace->slots[i].in_use = false;
    }

    keyspace->_next = NULL;
    if (!runtime_keyspaces_head) {
        runtime_keyspaces_head = keyspace;
    } else {
        struct zmk_custom_setting_keyspace *tail = runtime_keyspaces_head;
        while (tail->_next) {
            tail = tail->_next;
        }
        tail->_next = keyspace;
    }

    k_mutex_unlock(&settings_lock);
    return 0;
}

static char *keyspace_slot_key_buf(struct zmk_custom_setting_keyspace *keyspace, uint32_t index) {
    return &keyspace->keys[index * keyspace->max_key_len];
}

static bool keyspace_key_matches_prefix(const struct zmk_custom_setting_keyspace *keyspace,
                                        const char *key) {
    size_t prefix_len = bounded_strlen(keyspace->key_prefix, keyspace->max_key_len);
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

/* Find a slot index currently bound to `key` within keyspace. Caller must
 * hold settings_lock. */
static int keyspace_slot_index_for_key_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              const char *key) {
    for (uint32_t i = 0; i < keyspace->max_entries; i++) {
        if (keyspace->slots[i].in_use &&
            strncmp(keyspace_slot_key_buf(keyspace, i), key, keyspace->max_key_len) == 0) {
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

/* Fill in the slot's struct zmk_custom_setting from the keyspace's shared
 * metadata plus this slot's own key buffer. Caller must hold settings_lock
 * and have already copied `key` into the slot's key buffer. */
static void keyspace_init_slot_setting_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              uint32_t index) {
    struct zmk_custom_setting *setting = &keyspace->slots[index].setting;

    *setting = (struct zmk_custom_setting){
        .custom_subsystem_id = keyspace->custom_subsystem_id,
        .key = keyspace_slot_key_buf(keyspace, index),
        .array_key = NULL,
        .array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
        .array_state = NULL,
        .value_type = keyspace->value_type,
        .confidentiality = keyspace->confidentiality,
        .read_permission = keyspace->read_permission,
        .write_permission = keyspace->write_permission,
        .constraints = keyspace->constraints,
        .constraints_count = keyspace->constraints_count,
        .default_value = keyspace->default_value,
        .rpc_serializer = keyspace->rpc_serializer,
        .rpc_deserializer = keyspace->rpc_deserializer,
        .temp_slot = -1,
        .value_max_size = keyspace->max_size,
        /* A keyspace whose max_size exceeds the fixed carrier draws each
         * slot's region from the keyspace's shared pool on demand (issue
         * #16, exactly like ZMK_CUSTOM_SETTING_DEFINE_POOLED); otherwise
         * slots keep union storage (large_pool == NULL, large_data ==
         * NULL). large_data itself is left NULL here - a freshly (re)bound
         * slot starts with no region until it is written a non-empty value,
         * same as any pooled setting. */
        .large_pool = keyspace->large_pool,
    };
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

struct zmk_custom_setting *
zmk_custom_setting_keyspace_bind_locked(struct zmk_custom_setting_keyspace *keyspace,
                                        const char *key) {
    if (!keyspace || !key) {
        return NULL;
    }

    int existing_index = keyspace_slot_index_for_key_locked(keyspace, key);
    if (existing_index >= 0) {
        return &keyspace->slots[existing_index].setting;
    }

    size_t key_len = bounded_strlen(key, keyspace->max_key_len);
    if (key_len == 0 || key_len >= keyspace->max_key_len) {
        /* Key does not fit this keyspace's per-slot buffer: drop the
         * persisted record rather than truncating it silently. */
        return NULL;
    }

    int index = keyspace_free_slot_index_locked(keyspace);
    if (index < 0) {
        /* Pool exhausted. A reboot cannot fail because more entries were
         * persisted than max_entries allows (e.g. after lowering
         * max_entries) - the extra persisted records are simply left
         * unbound/unreachable until some are deleted and max_entries raised
         * again, rather than blocking boot. */
        return NULL;
    }

    memcpy(keyspace_slot_key_buf(keyspace, index), key, key_len + 1);
    keyspace_init_slot_setting_locked(keyspace, index);
    keyspace->slots[index].in_use = true;

    struct zmk_custom_setting *setting = &keyspace->slots[index].setting;
    /* register_runtime_locked (not zmk_custom_settings_register) - this
     * runs from inside custom_settings_handle_set while settings_load() is
     * already iterating persisted records, so a nested settings_load_subtree
     * call would be unsafe; the loaded value is applied by the caller
     * immediately after this returns instead. */
    register_runtime_locked(setting);
    return setting;
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

    if (value->type != keyspace->value_type) {
        return -EINVAL;
    }
    if ((value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
         value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) &&
        value->size > keyspace->max_size) {
        /* Per-keyspace value size ceiling, tighter than the global
         * CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE - see the field comment
         * on struct zmk_custom_setting_keyspace.max_size. */
        return -EMSGSIZE;
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

    memcpy(keyspace_slot_key_buf(keyspace, index), key, key_len + 1);
    keyspace_init_slot_setting_locked(keyspace, index);
    keyspace->slots[index].in_use = true;

    struct zmk_custom_setting *setting = &keyspace->slots[index].setting;
    register_runtime_locked(setting);

    k_mutex_unlock(&settings_lock);

    int ret = zmk_custom_setting_write(setting, value, mode);
    if (ret < 0) {
        /* Roll back: writing the initial value failed (e.g. constraint
         * violation), so do not leave a half-created entry occupying a
         * slot. */
        zmk_custom_settings_unregister(setting);
        k_mutex_lock(&settings_lock, K_FOREVER);
        keyspace->slots[index].in_use = false;
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
     * reset does, then unregister and free the slot. Ignore -ENOENT from a
     * setting that was never saved. */
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

    zmk_custom_settings_unregister(setting);

    k_mutex_lock(&settings_lock, K_FOREVER);
    keyspace->slots[index].in_use = false;
    k_mutex_unlock(&settings_lock);

    return 0;
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
            /* P5b: `next` may be a literal key previously created under a
             * registered keyspace's prefix (e.g. "macro/my-macro-1") whose
             * slot has not been (re)bound yet this boot - e.g. a fresh boot
             * loading a value persisted by a CreateSetting RPC in a
             * previous session, with no application code creating the
             * entry again. Auto-bind a slot for it here, then fall through
             * to apply this record's value to the freshly bound descriptor
             * exactly like any other setting - this is the "re-bound
             * during settings load" requirement: no module code has to run
             * for user-created entries to survive reboot. */
            k_mutex_lock(&settings_lock, K_FOREVER);
            struct zmk_custom_setting_keyspace *keyspace =
                keyspace_find_for_key_locked(custom_subsystem_id, next);
            if (keyspace) {
                setting = zmk_custom_setting_keyspace_bind_locked(keyspace, next);
            }
            k_mutex_unlock(&settings_lock);
        }
    }
    if (!setting) {
        return -ENOENT;
    }

    /* Sized to the largest per-setting value so a large-store setting's
     * persisted payload can be staged on load, not just the fixed carrier. */
    uint8_t data[CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE];
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
 * and zmk_custom_settings_register() (one runtime setting, registered after
 * boot) so the two initialization paths cannot drift apart. Caller must
 * hold settings_lock if called after boot (custom_settings_init runs before
 * any other thread can contend for it, so it does not bother). */
static void init_setting_state_locked(struct zmk_custom_setting *setting) {
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
        apply_scalar_default_locked(setting);
        setting->has_persistent_value = false;
        setting->dirty = false;
    }
    setting->temp_slot = -1;
    setting->temporary_active = false;
    setting->initialized = true;
}

static int custom_settings_init(void) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        init_setting_state_locked((struct zmk_custom_setting *)setting);
    }

    return 0;
}

SYS_INIT(custom_settings_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/*
 * P5a: runtime registration and the combined compile-time-section +
 * runtime-list iterator (ZMK_CUSTOM_SETTING_FOREACH in the header).
 */

static struct zmk_custom_setting *runtime_settings_head;

const struct zmk_custom_setting *zmk_custom_settings_foreach_first(void) {
    STRUCT_SECTION_FOREACH(zmk_custom_setting, first) { return first; }

    return runtime_settings_head;
}

const struct zmk_custom_setting *
zmk_custom_settings_foreach_next(const struct zmk_custom_setting *current) {
    if (!current) {
        return NULL;
    }

    /* Still inside the compile-time section: STRUCT_SECTION_ITERABLE
     * instances are laid out contiguously by the linker, so the "next"
     * compile-time entry is simply the next array slot - mirroring what
     * STRUCT_SECTION_FOREACH's own `iterator++` does - until the section
     * end is reached, at which point iteration continues into the runtime
     * list. */
    STRUCT_SECTION_START_EXTERN(zmk_custom_setting);
    STRUCT_SECTION_END_EXTERN(zmk_custom_setting);
    if (current >= STRUCT_SECTION_START(zmk_custom_setting) &&
        current < STRUCT_SECTION_END(zmk_custom_setting)) {
        const struct zmk_custom_setting *next = current + 1;
        if (next < STRUCT_SECTION_END(zmk_custom_setting)) {
            return next;
        }
        return runtime_settings_head;
    }

    /* Otherwise current is a runtime-registered setting; follow the
     * intrusive list. */
    return current->_runtime_next;
}

/* Shared by zmk_custom_settings_register() and the keyspace bind path
 * (custom_settings_handle_set, via zmk_custom_setting_keyspace_bind_locked):
 * link desc into the runtime list and initialize its mutable state, without
 * the settings_load_subtree() call that is only safe/meaningful outside an
 * active settings_load() pass (see the reentrancy note on
 * zmk_custom_settings_register below). Caller must hold settings_lock and
 * must have already checked for a duplicate subsystem+key. */
static void register_runtime_locked(struct zmk_custom_setting *desc) {
    desc->_runtime_next = NULL;
    init_setting_state_locked(desc);

    if (!runtime_settings_head) {
        runtime_settings_head = desc;
    } else {
        struct zmk_custom_setting *tail = runtime_settings_head;
        while (tail->_runtime_next) {
            tail = tail->_runtime_next;
        }
        tail->_runtime_next = desc;
    }
}

static bool runtime_setting_exists_locked(const char *custom_subsystem_id, const char *key) {
    ZMK_CUSTOM_SETTING_FOREACH(existing) {
        if (strncmp(existing->custom_subsystem_id, custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) == 0 &&
            strncmp(zmk_custom_setting_public_key(existing), key,
                    CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0) {
            return true;
        }
    }

    return false;
}

int zmk_custom_settings_register(struct zmk_custom_setting *desc) {
    if (!desc || !desc->custom_subsystem_id || (!desc->key && !desc->array_key)) {
        return -EINVAL;
    }

    /* ZMK_CUSTOM_SETTING_DEFINE_POOLED enforces this with a BUILD_ASSERT at
     * compile time; a hand-built or runtime-assembled descriptor that sets
     * large_pool has no such check, so enforce it here instead of silently
     * treating the setting as non-large-store (setting_uses_large_store()
     * also gates on type). */
    if (desc->large_pool != NULL && desc->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
        desc->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        return -EINVAL;
    }

    const char *lookup_key = zmk_custom_setting_is_array(desc) ? desc->array_key : desc->key;

    k_mutex_lock(&settings_lock, K_FOREVER);

    if (runtime_setting_exists_locked(desc->custom_subsystem_id, lookup_key)) {
        k_mutex_unlock(&settings_lock);
        return -EEXIST;
    }

    register_runtime_locked(desc);

    k_mutex_unlock(&settings_lock);

    /*
     * settings_load() (called once from main(), which always runs after
     * every SYS_INIT) may or may not have already happened by the time this
     * function runs, depending on whether the caller registers from its own
     * SYS_INIT (before) or from later application/driver code such as an
     * event handler (after) - there is no reliable flag to distinguish the
     * two from inside this module. Unconditionally loading just this
     * setting's own subtree resolves this without needing one:
     *   - If settings_load() already ran, this is the only chance for a
     *     previously-persisted value to reach this setting instead of it
     *     silently sticking at the default for the rest of the boot.
     *   - If settings_load() has not run yet, no store is registered yet
     *     either (that also happens in/around main()), so this call is a
     *     harmless no-op (SYS_SLIST_FOR_EACH_CONTAINER over an empty list of
     *     stores); settings_load() will pick up this setting normally once
     *     it runs, the same as any compile-time setting.
     * Either way the call is scoped to this one setting's storage name, so
     * it cannot trigger a larger reload of unrelated settings.
     *
     * This call is NOT safe to make while settings_load() itself is
     * iterating persisted records (e.g. from inside the
     * SETTINGS_STATIC_HANDLER_DEFINE callback below) - Zephyr's settings
     * core does not support a reentrant load while one is already in
     * progress. zmk_custom_setting_keyspace_bind_locked, the only caller
     * that can run in that context, uses register_runtime_locked directly
     * and skips this step: the record currently being loaded is applied by
     * the *caller* (custom_settings_handle_set) immediately after binding,
     * so no subtree reload is needed there anyway.
     */
    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(desc, name, sizeof(name));
    if (ret == 0) {
        settings_load_subtree(name);
    }

    return 0;
}

/* Release any pooled array_view slots belonging to array_state, clearing an
 * active temporary override each still owns and freeing the pool slot. Used
 * when the owning array descriptor is going away so its views cannot leak
 * temp slots (owner pointer into pool storage that is about to be reused) or
 * later alias a recycled pool entry. */
static void release_array_views_locked(const struct zmk_custom_setting_array_state *array_state) {
    for (size_t i = 0; i < ARRAY_SIZE(array_view_pool); i++) {
        struct zmk_custom_setting_array_view_slot *slot = &array_view_pool[i];
        if (slot->in_use && slot->view.array_state == array_state) {
            clear_temporary_locked(&slot->view);
            slot->in_use = false;
        }
    }
}

int zmk_custom_settings_unregister(struct zmk_custom_setting *desc) {
    if (!desc) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);

    /* Release any temporary override slot owned by this descriptor (or, for
     * an array, by any of its live element views) before detaching it. The
     * descriptor's storage may be freed/reused by the caller after this
     * returns, and temp/view slots track ownership by pointer - leaving a
     * stale owner would leak the slot and could mis-attribute a later
     * temp_slot_alloc collision or clear_temporary walk. */
    if (zmk_custom_setting_is_array(desc) && desc->array_state) {
        release_array_views_locked(desc->array_state);
    } else {
        clear_temporary_locked(desc);
    }

    if (runtime_settings_head == desc) {
        runtime_settings_head = desc->_runtime_next;
        desc->_runtime_next = NULL;
        /* Release any pool region before this descriptor's storage is
         * freed/reused by the caller - with the pool's intrusive member
         * list (see struct zmk_custom_setting_large_pool), leaving `desc`
         * linked in would corrupt the list the moment its memory is
         * repurposed (e.g. a keyspace slot rebound to a different key). */
        pool_release_locked(desc);
        k_mutex_unlock(&settings_lock);
        return 0;
    }

    struct zmk_custom_setting *prev = runtime_settings_head;
    while (prev && prev->_runtime_next != desc) {
        prev = prev->_runtime_next;
    }

    if (!prev) {
        k_mutex_unlock(&settings_lock);
        return -ENOENT;
    }

    prev->_runtime_next = desc->_runtime_next;
    desc->_runtime_next = NULL;
    pool_release_locked(desc);

    k_mutex_unlock(&settings_lock);
    return 0;
}
