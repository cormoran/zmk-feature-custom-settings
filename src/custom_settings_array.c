/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/* Array lifecycle uses caller-owned views; the core has no view cache. */

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
#include "custom_settings_array_storage.h"

/* The caller owns this view. It must not escape a synchronous operation. */
struct zmk_custom_setting *array_view_init(struct zmk_custom_setting *view,
                                           const struct zmk_custom_setting *array, uint32_t index) {
    if (!zmk_custom_setting_is_array(array) || index >= array->array_state->max_size) {
        return NULL;
    }
    *view = *array;
    view->array_index = index;
    view->default_value = NULL;
    return view;
}

struct zmk_custom_setting *array_view_find(struct zmk_custom_setting *view, const char *subsystem,
                                           const char *key, uint32_t index) {
    return array_view_init(view, zmk_custom_setting_find_array(subsystem, key), index);
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

        if (strncmp(zmk_custom_setting_public_key(setting), key,
                    CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0) {
            return setting;
        }
    }

    return NULL;
}

static int validate_array_size(const struct zmk_custom_setting *setting, uint32_t array_size) {
    if (!zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    return array_size <= setting->array_state->max_size ? 0 : -ERANGE;
}

/* Inactive indices cannot retain temporary overrides. */
static void
clear_temporary_past_size_locked(const struct zmk_custom_setting_array_state *array_state,
                                 uint32_t new_size) {
    clear_array_temporary_locked(array_state, new_size);
}

/* Non-static: zmk_custom_setting_discard/zmk_custom_setting_reset
 * (custom_settings.c) call these two setters directly for whole-array
 * discard/reset. Declared in custom_settings_internal.h. */
void set_array_memory_size_locked(const struct zmk_custom_setting *array_element,
                                  uint32_t array_size) {
    struct zmk_custom_setting_array_state *array_state = array_element->array_state;

    array_state->size = array_size;
    clear_temporary_past_size_locked(array_state, array_size);
}

void set_array_persistent_size_locked(const struct zmk_custom_setting *array_element,
                                      uint32_t array_size) {
    array_element->array_state->persistent_size = array_size;
}

uint32_t zmk_custom_setting_array_size(const struct zmk_custom_setting *setting) {
    if (!zmk_custom_setting_is_array(setting)) {
        return 0;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    uint32_t array_size = setting->array_state->size;
    k_mutex_unlock(&custom_settings_lock);

    return array_size;
}

uint32_t zmk_custom_setting_array_max_size(const struct zmk_custom_setting *setting) {
    return zmk_custom_setting_is_array(setting) ? setting->array_state->max_size : 0;
}

/* Non-static: zmk_custom_setting_reset (custom_settings.c) calls this
 * directly to erase the "_size" marker on discard/reset. Declared in
 * custom_settings_internal.h. */
int array_size_storage_name(const struct zmk_custom_setting *setting, char *name,
                            size_t name_size) {
    if (!zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    int ret = snprintf(name, name_size, SETTINGS_SUBTREE "/%s/%s/%s", setting->custom_subsystem_id,
                       zmk_custom_setting_public_key(setting), ARRAY_SIZE_STORAGE_KEY);
    if (ret < 0 || ret >= name_size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

/* Delete on-flash records for slots at or past array_size. Operates
 * directly on the descriptor's array_state buffer (O(max_size), bounded by
 * the array's own max element count). */
static int delete_inactive_array_values_locked(const struct zmk_custom_setting *array_descriptor,
                                               uint32_t array_size) {
    struct zmk_custom_setting_array_state *array_state = array_descriptor->array_state;

    for (uint32_t index = array_size; index < array_state->max_size; index++) {
        if (!array_flag_get(array_state->has_persistent, index)) {
            continue;
        }

        struct zmk_custom_setting view_storage;
        struct zmk_custom_setting *view =
            array_view_init(&view_storage, (struct zmk_custom_setting *)array_descriptor, index);
        char name[SETTINGS_MAX_NAME_LEN];
        int ret = setting_storage_name(view, name, sizeof(name));
        if (ret < 0) {
            return ret;
        }

        ret = settings_delete(name);
        if (ret != 0 && ret != -ENOENT) {
            return ret;
        }

        array_flag_set(array_state->has_persistent, index, false);
        array_flag_set(array_state->dirty, index, true);
    }

    return 0;
}

/* Save all active elements (indices [0, array_size)) plus the "_size"
 * marker, then delete any now-inactive elements. Operates directly on the
 * descriptor's array_state buffer, so cost is O(array_size) rather than a
 * walk of the full setting registry. Non-static: save_setting_locked
 * (custom_settings.c) calls this directly for an array setting. Declared in
 * custom_settings_internal.h. */
int save_array_locked(const struct zmk_custom_setting *array_descriptor) {
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
        struct zmk_custom_setting view_storage;
        struct zmk_custom_setting *view =
            array_view_init(&view_storage, (struct zmk_custom_setting *)array_descriptor, index);
        char name[SETTINGS_MAX_NAME_LEN];
        ret = setting_storage_name(view, name, sizeof(name));
        if (ret < 0) {
            return ret;
        }

        const void *data;
        size_t len;
        struct zmk_custom_setting_value_view value;
        array_value_read(array_descriptor, index, &value);
        ret = value_to_storage(&value, &data, &len);
        if (ret < 0) {
            return ret;
        }

        ret = settings_save_one(name, data, len);
        if (ret < 0) {
            return ret;
        }

        array_flag_set(array_state->has_persistent, index, true);
        array_flag_set(array_state->dirty, index, false);
    }

    set_array_persistent_size_locked(array_descriptor, array_size);
    return delete_inactive_array_values_locked(array_descriptor, array_size);
}

int zmk_custom_setting_read_array_by_key_view(const char *custom_subsystem_id, const char *key,
                                              uint32_t index,
                                              struct zmk_custom_setting_value_view *value) {
    struct zmk_custom_setting setting_storage;
    const struct zmk_custom_setting *setting =
        array_view_find(&setting_storage, custom_subsystem_id, key, index);
    if (!setting) {
        return -ENOENT;
    }

    return zmk_custom_setting_read_view(setting, value);
}

int zmk_custom_setting_write_array_by_key_view(const char *custom_subsystem_id, const char *key,
                                               uint32_t index,
                                               const struct zmk_custom_setting_value_view *value,
                                               enum zmk_custom_setting_write_mode mode) {
    struct zmk_custom_setting setting_storage;
    const struct zmk_custom_setting *setting =
        array_view_find(&setting_storage, custom_subsystem_id, key, index);
    if (!setting) {
        return -ENOENT;
    }

    return zmk_custom_setting_write_view(setting, value, mode);
}

int zmk_custom_setting_write_array_element_view(const struct zmk_custom_setting *const_setting,
                                                const struct zmk_custom_setting_value_view *value,
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

    ret = zmk_custom_setting_validate_view(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    uint32_t old_size = setting->array_state->size;
    ret = write_value_locked(setting, value,
                             mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                 ? ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY
                                 : mode);
    if (ret == 0) {
        set_array_memory_size_locked(setting, array_size);
        if (mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) {
            ret = save_setting_locked(setting);
        }
    } else {
        setting->array_state->size = old_size;
    }

    k_mutex_unlock(&custom_settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_array_push_back_view(const struct zmk_custom_setting *setting,
                                            const struct zmk_custom_setting_value_view *value,
                                            enum zmk_custom_setting_write_mode mode) {
    if (!setting || !value || !zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    uint32_t array_size = setting->array_state->size;
    uint32_t array_max_size = setting->array_state->max_size;
    k_mutex_unlock(&custom_settings_lock);

    if (array_size >= array_max_size) {
        return -ERANGE;
    }

    struct zmk_custom_setting tail_storage;
    const struct zmk_custom_setting *tail =
        array_view_find(&tail_storage, setting->custom_subsystem_id,
                        zmk_custom_setting_public_key(setting), array_size);
    if (!tail) {
        return -ENOENT;
    }

    return zmk_custom_setting_write_array_element_view(tail, value, array_size + 1, mode);
}

int zmk_custom_setting_array_pop_back_view(const struct zmk_custom_setting *const_setting,
                                           struct zmk_custom_setting_value_view *value,
                                           enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !zmk_custom_setting_is_array(const_setting)) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    uint32_t array_size = setting->array_state->size;
    if (array_size == 0) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOENT;
    }
    k_mutex_unlock(&custom_settings_lock);

    struct zmk_custom_setting tail_const_storage;
    const struct zmk_custom_setting *tail_const =
        array_view_find(&tail_const_storage, setting->custom_subsystem_id,
                        zmk_custom_setting_public_key(setting), array_size - 1);
    if (!tail_const) {
        return -ENOENT;
    }

    struct zmk_custom_setting *tail = (struct zmk_custom_setting *)tail_const;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    if (value) {
        int copy_ret = copy_value(value, effective_value(tail));
        if (copy_ret) {
            k_mutex_unlock(&custom_settings_lock);
            return copy_ret;
        }
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

    k_mutex_unlock(&custom_settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_array_insert_at_view(const struct zmk_custom_setting *const_setting,
                                            uint32_t index,
                                            const struct zmk_custom_setting_value_view *value,
                                            enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !value || !zmk_custom_setting_is_array(const_setting)) {
        return -EINVAL;
    }

    if (mode != ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY &&
        mode != ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;
    int ret = zmk_custom_setting_validate_view(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    struct zmk_custom_setting_array_state *array_state = setting->array_state;
    uint32_t array_size = array_state->size;

    if (index > array_size) {
        k_mutex_unlock(&custom_settings_lock);
        return -ERANGE;
    }
    if (array_size >= array_state->max_size) {
        k_mutex_unlock(&custom_settings_lock);
        return -ERANGE;
    }

    /* Stage the new element in the inactive tail before shifting. Pool
     * exhaustion must leave every active element and flag unchanged. */
    ret = array_value_write(setting, array_size, value);
    if (ret < 0) {
        k_mutex_unlock(&custom_settings_lock);
        return ret;
    }
    for (uint32_t i = array_size; i > index; --i) {
        array_value_swap(setting, i, i - 1);
    }
    for (uint32_t i = index; i <= array_size; ++i) {
        array_flag_set(array_state->dirty, i, true);
    }
    clear_temporary_past_size_locked(array_state, index);
    array_state->size = array_size + 1;

    struct zmk_custom_setting view_storage;
    struct zmk_custom_setting *view = array_view_init(&view_storage, setting, index);
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
    k_mutex_unlock(&custom_settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

int zmk_custom_setting_array_remove_at_view(const struct zmk_custom_setting *const_setting,
                                            uint32_t index,
                                            struct zmk_custom_setting_value_view *out_value,
                                            enum zmk_custom_setting_write_mode mode) {
    if (!const_setting || !zmk_custom_setting_is_array(const_setting)) {
        return -EINVAL;
    }

    if (mode != ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY &&
        mode != ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    struct zmk_custom_setting_array_state *array_state = setting->array_state;
    uint32_t array_size = array_state->size;

    if (index >= array_size) {
        k_mutex_unlock(&custom_settings_lock);
        return -ENOENT;
    }

    if (out_value) {
        struct zmk_custom_setting removed_view_storage;
        struct zmk_custom_setting *removed_view =
            array_view_init(&removed_view_storage, setting, index);
        int copy_ret = copy_value(out_value, effective_value(removed_view));
        if (copy_ret) {
            k_mutex_unlock(&custom_settings_lock);
            return copy_ret;
        }
    }
    clear_temporary_past_size_locked(array_state, index);

    for (uint32_t i = index; i + 1 < array_size; ++i) {
        array_value_swap(setting, i, i + 1);
        array_flag_set(array_state->dirty, i, true);
    }
    array_value_default(setting, array_size - 1);
    array_state->size = array_size - 1;
    array_flag_set(array_state->dirty, array_size - 1, true);

    struct zmk_custom_setting tail_view_storage;
    struct zmk_custom_setting *tail_view =
        array_view_init(&tail_view_storage, setting, array_size - 1);
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
    k_mutex_unlock(&custom_settings_lock);

    if (ret == 0) {
        raise_setting_changed(setting, mode == ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                           ? ZMK_CUSTOM_SETTING_CHANGED_SAVED
                                           : ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED);
    }

    return ret;
}

/* Discard one array element view's in-memory value back to persisted/
 * default, and (once per call, harmless if repeated) restore the whole
 * array's active length to its persisted length. Called once per active
 * element by apply_scope so zmk_custom_settings_discard_scope's
 * affected_count matches "one per active element". Non-static:
 * zmk_custom_setting_discard
 * (custom_settings.c) calls this directly per active element. Declared in
 * custom_settings_internal.h. */
int discard_array_element_locked(struct zmk_custom_setting *view) {
    struct zmk_custom_setting_array_state *array_state = view->array_state;
    uint32_t index = view->array_index;

    set_array_memory_size_locked(view, array_state->persistent_size);

    if (array_flag_get(array_state->has_persistent, index)) {
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
            array_value_default(view, index);
        }
    } else {
        array_value_default(view, index);
    }
    array_flag_set(array_state->dirty, index, false);
    clear_temporary_locked(view);
    return 0;
}

/* Reset one array element view's persisted/in-memory value to its
 * compile-time default. Used both directly (whole-array reset) and once per
 * active element by apply_scope. Non-static: zmk_custom_setting_reset
 * (custom_settings.c) calls this directly per active element. Declared in
 * custom_settings_internal.h. */
int reset_array_element_locked(struct zmk_custom_setting *view) {
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

    array_value_default(view, index);
    array_flag_set(array_state->has_persistent, index, false);
    array_flag_set(array_state->dirty, index, false);
    clear_temporary_locked(view);
    return 0;
}

/* Non-static: custom_settings_handle_set (custom_settings.c) calls this
 * directly to apply a loaded "_size" record. Declared in
 * custom_settings_internal.h. */
int array_size_from_storage(const struct zmk_custom_setting *array_element, const void *data,
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

/* Non-static: custom_settings_handle_set (custom_settings.c) calls this to
 * recognize a loaded "_size" record. Declared in custom_settings_internal.h. */
bool split_array_size_key(const char *name, char *array_key, size_t array_key_size) {
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
 * ("array_value") and numeric index (2). Array elements are not individually
 * registered (see ZMK_CUSTOM_SETTING_ARRAY_DEFINE), so
 * custom_settings_handle_set cannot resolve them with a plain
 * zmk_custom_setting_find(name) lookup and needs this to route into
 * zmk_custom_setting_find_array_element instead. Non-static:
 * custom_settings_handle_set (custom_settings.c) calls this to recognize a
 * loaded per-element record. Declared in custom_settings_internal.h. */
bool split_array_element_key(const char *name, char *array_key, size_t array_key_size,
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
