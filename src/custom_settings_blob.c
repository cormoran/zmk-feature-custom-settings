/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

#include "custom_settings_internal.h"

static uint8_t shared_bytes[CONFIG_ZMK_CUSTOM_SETTINGS_POOL_SIZE];
struct zmk_custom_setting_large_pool zmk_custom_settings_shared_pool = {
    .data = shared_bytes,
    .size = sizeof(shared_bytes),
};

uint8_t custom_settings_edit_bytes[CUSTOM_SETTINGS_EDIT_SIZE];

int blob_write_locked(struct zmk_custom_setting_large_pool *pool,
                      struct zmk_custom_setting_blob *blob, const void *data, size_t size,
                      bool terminate) {
    if (size > sizeof(custom_settings_edit_bytes) - (terminate ? 1 : 0) || (size && !data)) {
        return -EMSGSIZE;
    }
    /* Snapshot first: data may refer to this or another movable pool node.
     * memmove also permits a caller to assemble directly in the workspace. */
    if (size) {
        memmove(custom_settings_edit_bytes, data, size);
    }
    int ret = custom_settings_pool_reserve(pool, blob, size + (terminate ? 1 : 0));
    if (ret < 0) {
        return ret;
    }
    if (size) {
        memcpy(blob->data, custom_settings_edit_bytes, size);
    }
    if (terminate) {
        blob->data[size] = '\0';
    }
    blob->size = size;
    return 0;
}

void pool_release_locked(const struct zmk_custom_setting *setting) {
    if (setting->blob.pool) {
        custom_settings_pool_release(setting->blob.pool,
                                     zmk_custom_setting_state_blob(setting->state));
    }
}

size_t zmk_custom_setting_large_pool_used(const struct zmk_custom_setting_large_pool *pool) {
    if (!pool) {
        return 0;
    }
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    size_t used = custom_settings_pool_used(pool);
    k_mutex_unlock(&custom_settings_lock);
    return used;
}

int zmk_custom_setting_with_large_raw_bytes(const struct zmk_custom_setting *setting,
                                            zmk_custom_setting_raw_bytes_visitor_t visitor,
                                            void *user_data) {
    if (!setting || !visitor) {
        return -EINVAL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (!setting_uses_blob_store(setting) || setting_temporary_active(setting)) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOTSUP;
    }

    struct zmk_custom_setting_state *state = setting->state;

    if (setting->_keyspace) {
        /* Stream only the payload slice - skip the embedded
         * "user_key\0" prefix (see the keyspace design comment in the
         * header). Re-derived under the lock every call, same invariant as
         * every other blob.data access in this file. */
        size_t key_len = keyspace_blob_key_len_locked(setting);
        const uint8_t *payload = zmk_custom_setting_state_blob(state)->size > key_len
                                     ? zmk_custom_setting_state_blob(state)->data + key_len + 1
                                     : (const uint8_t *)"";
        size_t payload_size = zmk_custom_setting_state_blob(state)->size > key_len
                                  ? zmk_custom_setting_state_blob(state)->size - key_len - 1
                                  : 0;
        visitor(payload, payload_size, user_data);
        k_mutex_unlock(&custom_settings_lock);
        return 0;
    }

    const uint8_t *data = zmk_custom_setting_state_blob(state)->size > 0
                              ? zmk_custom_setting_state_blob(state)->data
                              : (const uint8_t *)"";
    visitor(data, zmk_custom_setting_state_blob(state)->size, user_data);
    k_mutex_unlock(&custom_settings_lock);
    return 0;
}

/* Apply a large (> carrier) raw BYTES/STRING payload to a blob setting.
 * Caller holds custom_settings_lock. TEMPORARY mode is not supported for
 * large values (the temporary override pool is intentionally small). */
int write_large_locked(const struct zmk_custom_setting *setting, const void *data, size_t size,
                       enum zmk_custom_setting_write_mode mode) {
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
        ret = persist_raw_candidate_locked(setting, data, size);
        if (ret < 0) {
            return ret;
        }
        ret = blob_store_set_raw(setting, data, size);
        if (ret == 0) {
            clear_temporary_locked(setting);
            set_setting_dirty(setting, false);
        }
        return ret;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY:
        return -EMSGSIZE;
    default:
        return -EINVAL;
    }
}
