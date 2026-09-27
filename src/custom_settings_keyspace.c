/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * RPC-creatable keyspaces / namespaces (CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE,
 * which selects CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUES). A keyspace slot's
 * entire entry - its user key *and* its payload - is one opaque pool-backed
 * BYTES blob, so this feature cannot link without the pool
 * (custom_settings_blob.c / custom_settings_allocator.c).
 *
 * This file owns key lookup and payload presentation. Core and ref code
 * dispatch keyspace operations here; storage and the allocator see pooled
 * bytes, regardless of the payload type presented to callers. Core call
 * sites guard their use of this file's entry points with
 * zmk_custom_setting_keyspace_of(), which folds to a compile-time constant
 * NULL when the feature is off, so none of these functions are reachable in a
 * CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE=n build.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <cormoran/zmk/custom_settings.h>

#include "custom_settings_internal.h"

static bool keyspace_key_matches_prefix(const struct zmk_custom_setting_keyspace *keyspace,
                                        const char *key) {
    size_t prefix_len = strlen(keyspace->key_prefix);
    return strncmp(key, keyspace->key_prefix, prefix_len) == 0;
}

/* Find a keyspace registered for custom_subsystem_id whose key_prefix
 * matches the start of `key`. Caller must hold custom_settings_lock.
 *
 * A NULL or empty custom_subsystem_id means "match any subsystem", exactly
 * as zmk_custom_setting_find() treats it: an RPC caller that addresses a
 * keyspace entry by key alone - a SettingRef with no custom_subsystem_index,
 * which is how the runtime-macro web UI issues CreateSetting/DeleteSetting -
 * lands here with a NULL id and must still resolve the keyspace by its key
 * prefix. Without this the create/delete handlers reported "No keyspace
 * registered for this key" for every such request. */
static struct zmk_custom_setting_keyspace *
keyspace_find_for_key_locked(const char *custom_subsystem_id, const char *key) {
    ZMK_CUSTOM_SETTING_KEYSPACE_FOREACH(keyspace) {
        if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
            strncmp(keyspace->custom_subsystem_id, custom_subsystem_id,
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
    if (!key) {
        return NULL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    struct zmk_custom_setting_keyspace *keyspace =
        keyspace_find_for_key_locked(custom_subsystem_id, key);
    k_mutex_unlock(&custom_settings_lock);

    return keyspace;
}

/* Scratch destination for a keyspace slot's decoded user key. Safe as a
 * single shared instance: populated and consumed synchronously by the caller
 * under the lock, not valid past the immediate call. */
static char keyspace_public_key_scratch[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];

const char *keyspace_public_key_locked(const struct zmk_custom_setting *setting) {
    size_t key_len = keyspace_blob_key_len_locked(setting);
    key_len = MIN(key_len, sizeof(keyspace_public_key_scratch) - 1);
    if (zmk_custom_setting_state_blob(setting->state)->data != NULL && key_len > 0) {
        memcpy(keyspace_public_key_scratch, zmk_custom_setting_state_blob(setting->state)->data,
               key_len);
    }
    keyspace_public_key_scratch[key_len] = '\0';
    return keyspace_public_key_scratch;
}

/* Find the slot index currently bound to `key`, decoding each live slot's
 * blob to compare (a short walk over the live slots, not a scan of raw pool
 * bytes). Caller holds custom_settings_lock. */
static int keyspace_slot_index_for_key_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              const char *key) {
    size_t key_len = strlen(key);
    for (uint32_t i = 0; i < keyspace->max_entries; i++) {
        if (!keyspace->slots[i].in_use) {
            continue;
        }
        const struct zmk_custom_setting *slot_setting = &keyspace->slots[i].setting;
        size_t blob_key_len = keyspace_blob_key_len_locked(slot_setting);
        if (blob_key_len == key_len &&
            zmk_custom_setting_state_blob(slot_setting->state)->data != NULL &&
            memcmp(zmk_custom_setting_state_blob(slot_setting->state)->data, key, key_len) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int keyspace_free_slot_index_locked(struct zmk_custom_setting_keyspace *keyspace) {
    for (uint32_t i = 0; i < keyspace->max_entries; i++) {
        if (!keyspace->slots[i].in_use && keyspace->slots[i].generation != UINT32_MAX) {
            return (int)i;
        }
    }

    return -1;
}

/* Bind slot `index` of `keyspace` to its stable ordinal storage identity
 * ("<key_prefix>#<index>") and wire it into the keyspace's shared pool, with
 * an EMPTY blob (blob.data == NULL) - the caller populates it immediately
 * after, either via the settings-load value-apply step (a persisted record)
 * or keyspace_write_blob (a fresh create). Caller holds custom_settings_lock.
 * Never fails: the ordinal name buffer is sized for any key_prefix +
 * max_entries this module's BUILD_ASSERTs allow. */
struct zmk_custom_setting *keyspace_bind_slot_locked(struct zmk_custom_setting_keyspace *keyspace,
                                                     uint32_t index) {
    struct zmk_custom_setting_keyspace_slot *slot = &keyspace->slots[index];

    if (slot->generation == UINT32_MAX) {
        return NULL; /* Retire instead of making an old ref valid again. */
    }
    slot->generation++;

    /* Reset the slot's embedded state block first: a freshly bound slot
     * starts with an empty blob (data == NULL - its region is carved from the
     * pool on the first write/load), no flags, no temp slot, and no default
     * override left over from a previous occupant. */
    memset(&slot->state, 0, sizeof(slot->state));
    slot->setting = (struct zmk_custom_setting){
        .custom_subsystem_id = keyspace->custom_subsystem_id,
        .key = keyspace->key_prefix,
        ZMK_CUSTOM_SETTING_ARRAY_KEY_INIT(NULL).array_index = ZMK_CUSTOM_SETTING_ARRAY_NONE,
        .value_type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .confidentiality = keyspace->confidentiality,
        .read_permission = keyspace->read_permission,
        .write_permission = keyspace->write_permission,
        /* Deliberately NOT keyspace->constraints/rpc_*: those describe the
         * PAYLOAD, not the slot's own opaque BYTES blob, which has no
         * constraints of its own. */

        .constraints_count = 0,
        .default_value = NULL,

        .blob = {.max_size = keyspace->max_key_len + keyspace->max_size,
                 .pool = keyspace->large_pool},
        ._keyspace = keyspace,
        .state = ZMK_CUSTOM_SETTING_STATE_HEADER(slot->state),
    };
    slot->in_use = true;
    init_setting_state_locked(&slot->setting);
    return &slot->setting;
}

/* Release a live slot back to the pool: clears any temporary override,
 * releases its pool region, and marks it free. Caller holds custom_settings_lock. */
void keyspace_release_slot_locked(struct zmk_custom_setting_keyspace *keyspace, uint32_t index) {
    struct zmk_custom_setting_keyspace_slot *slot = &keyspace->slots[index];
    clear_temporary_locked(&slot->setting);
    pool_release_locked(&slot->setting);
    slot->in_use = false;
}

/* Release the live slot that `setting` is the descriptor of. `setting` must be
 * a keyspace slot's own embedded descriptor (&keyspace->slots[i].setting); the
 * owning index is recovered by pointer arithmetic on the fixed slot array.
 * Used by reset, which unlike delete only has the descriptor in hand, not the
 * user key. Caller holds custom_settings_lock. */
void keyspace_release_slot_for_setting_locked(struct zmk_custom_setting_keyspace *keyspace,
                                              const struct zmk_custom_setting *setting) {
    const struct zmk_custom_setting_keyspace_slot *slot =
        CONTAINER_OF(setting, struct zmk_custom_setting_keyspace_slot, setting);
    keyspace_release_slot_locked(keyspace, (uint32_t)(slot - keyspace->slots));
}

/* Validate a PAYLOAD (not a slot's opaque blob) against keyspace->value_type/
 * constraints/max_size - shared by zmk_custom_setting_keyspace_create_view and
 * zmk_custom_setting_write_view's keyspace branch. */
int keyspace_validate_payload(const struct zmk_custom_setting_keyspace *keyspace,
                              const struct zmk_custom_setting_value_view *value) {
#ifndef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
    const struct zmk_custom_setting_metadata metadata = {.constraints = keyspace->constraints};
#endif
    struct zmk_custom_setting payload_shape = {
        .value_type = keyspace->value_type,
        .blob.max_size = keyspace->max_size,
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
        .constraints = keyspace->constraints,
#else
        .metadata = &metadata,
#endif
        .constraints_count = keyspace->constraints_count,
    };
    int ret = zmk_custom_setting_validate_view(&payload_shape, value);
    if (ret < 0) {
        return ret;
    }

    if ((value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
         value->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) &&
        value->size > keyspace->max_size) {
        /* Per-keyspace value size ceiling. */
        return -EMSGSIZE;
    }

    return 0;
}

/* Shared blob assembly for keyspace_write_blob (typed payload) and
 * keyspace_write_raw_payload (already-raw payload): builds
 * `blob = [key\0][payload]` and writes it via write_bytes_raw (the
 * keyspace-agnostic pooled-BYTES path). `key` is the literal user key for a
 * fresh create, or NULL to reuse the slot's current key (a write to an
 * already-live entry). */
static int keyspace_write_raw_payload_with_key(const struct zmk_custom_setting *setting,
                                               const char *key, const void *payload_data,
                                               size_t payload_len,
                                               enum zmk_custom_setting_write_mode mode) {
    char key_copy[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (!key) {
        key = keyspace_public_key_locked(setting);
    }
    size_t key_len = bounded_strlen(key, sizeof(key_copy));
    if (key_len >= sizeof(key_copy) || payload_len > CUSTOM_SETTINGS_EDIT_SIZE - key_len - 1) {
        k_mutex_unlock(&custom_settings_lock);
        return -EMSGSIZE;
    }
    memcpy(key_copy, key, key_len + 1);
    /* Payload may already be in this workspace (record encode), or may
     * alias the live pool. Copy it before writing the prefix. */
    if (payload_len) {
        memmove(custom_settings_edit_bytes + key_len + 1, payload_data, payload_len);
    }
    memcpy(custom_settings_edit_bytes, key_copy, key_len + 1);
    int ret = write_bytes_raw(setting, custom_settings_edit_bytes, key_len + 1 + payload_len, mode);
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}

/* `value` (typed as keyspace->value_type) is converted to raw payload bytes
 * via value_to_storage, then assembled/written via
 * keyspace_write_raw_payload_with_key. Caller has already validated `value`
 * via keyspace_validate_payload. */
int keyspace_write_blob(const struct zmk_custom_setting *setting, const char *key,
                        const struct zmk_custom_setting_value_view *value,
                        enum zmk_custom_setting_write_mode mode) {
    const void *payload_data;
    size_t payload_len;
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int ret = value_to_storage(value, &payload_data, &payload_len);
    if (!ret)
        ret = keyspace_write_raw_payload_with_key(setting, key, payload_data, payload_len, mode);
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}

int keyspace_write_raw_payload(const struct zmk_custom_setting *setting, const void *data,
                               size_t size, enum zmk_custom_setting_write_mode mode) {
    const struct zmk_custom_setting_keyspace *keyspace = setting->_keyspace;
    if (keyspace->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES &&
        keyspace->value_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        /* A raw-bytes write only makes sense for a BYTES/STRING-declared
         * keyspace; other payload types must go through
         * zmk_custom_setting_write_view with a typed value. */
        return -EINVAL;
    }
    if (size > keyspace->max_size) {
        return -EMSGSIZE;
    }

    return keyspace_write_raw_payload_with_key(setting, NULL, data, size, mode);
}

/* Borrow the payload under the settings lock, stripping [key NUL]. Behavior
 * storage is decoded into caller-owned memory that lives through the visitor. */
int keyspace_payload_view_locked(const struct zmk_custom_setting *setting,
                                 struct zmk_custom_setting_value_view *out_value,
                                 struct zmk_custom_setting_behavior_value *behavior) {
    const struct zmk_custom_setting_value_view *blob = effective_value(setting);
    if (!blob)
        return -ENOENT;
    const uint8_t *nul = blob->size ? memchr(blob->bytes_value, '\0', blob->size) : NULL;
    if (!nul)
        return -EINVAL;
    size_t prefix = (size_t)(nul - blob->bytes_value) + 1;
    size_t size = blob->size - prefix;
    enum zmk_custom_setting_value_type type = setting->_keyspace->value_type;
    if ((type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 && size != sizeof(int32_t)) ||
        (type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL && size != sizeof(bool)))
        return -EINVAL;
    return value_from_raw(out_value, type, blob->bytes_value + prefix, size, behavior);
}

int keyspace_read_payload(const struct zmk_custom_setting *setting,
                          struct zmk_custom_setting_value_view *out_value) {
    struct zmk_custom_setting_value_view payload;
    struct zmk_custom_setting_behavior_value behavior;
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int ret = keyspace_payload_view_locked(setting, &payload, &behavior);
    if (!ret)
        ret = copy_value(out_value, &payload);
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}

/* Copy the presented payload while its backing region is still locked. */
int keyspace_read_into(const struct zmk_custom_setting *setting, void *buf, size_t capacity,
                       size_t *out_size, enum zmk_custom_setting_value_type *out_type) {
    struct read_into_context ctx = {.buf = buf, .capacity = capacity};
    int ret = zmk_custom_setting_with_view(setting, read_into_visitor, &ctx);
    if (ret || ctx.ret)
        return ret ? ret : ctx.ret;
    if (out_size)
        *out_size = ctx.out_size;
    if (out_type)
        *out_type = ctx.out_type;
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

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int index = keyspace_slot_index_for_key_locked(mutable_keyspace, key);
    k_mutex_unlock(&custom_settings_lock);

    return index >= 0 ? &keyspace->slots[index].setting : NULL;
}

int zmk_custom_setting_keyspace_create_view(struct zmk_custom_setting_keyspace *keyspace,
                                            const char *key,
                                            const struct zmk_custom_setting_value_view *value,
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

    k_mutex_lock(&custom_settings_lock, K_FOREVER);

    if (keyspace_slot_index_for_key_locked(keyspace, key) >= 0) {
        k_mutex_unlock(&custom_settings_lock);
        return -EEXIST;
    }

    int index = keyspace_free_slot_index_locked(keyspace);
    if (index < 0) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOSPC;
    }

    struct zmk_custom_setting *setting = keyspace_bind_slot_locked(keyspace, (uint32_t)index);
    k_mutex_unlock(&custom_settings_lock);

    ret = keyspace_write_blob(setting, key, value, mode);
    if (ret < 0) {
        /* Roll back: the initial value write failed (e.g. constraint
         * violation, or -ENOSPC from the pool), so do not leave a
         * half-created entry occupying a slot. */
        k_mutex_lock(&custom_settings_lock, K_FOREVER);
        keyspace_release_slot_locked(keyspace, (uint32_t)index);
        k_mutex_unlock(&custom_settings_lock);
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

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int index = keyspace_slot_index_for_key_locked(keyspace, key);
    if (index < 0) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOENT;
    }
    struct zmk_custom_setting *setting = &keyspace->slots[index].setting;
    k_mutex_unlock(&custom_settings_lock);

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

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    keyspace_release_slot_locked(keyspace, (uint32_t)index);
    k_mutex_unlock(&custom_settings_lock);

    return 0;
}

/* Parse a stored record name's remainder (after the custom_subsystem_id
 * component split off by settings_name_next, e.g. "macro/#3" for a keyspace
 * whose key_prefix is "macro/") as "<key_prefix>#<index>" for `keyspace`.
 * Zephyr's settings subsystem only treats '/' as a hierarchy separator -
 * '#' is an ordinary name byte, so this format needs no escaping. */
bool keyspace_parse_ordinal_name(const struct zmk_custom_setting_keyspace *keyspace,
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
