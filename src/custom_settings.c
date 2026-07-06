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

static const struct zmk_custom_setting_value *
effective_value(const struct zmk_custom_setting *setting) {
    if (setting->temporary_active && setting->temp_slot >= 0) {
        const struct zmk_custom_settings_temp_slot *slot = &temp_slots[setting->temp_slot];
        value_from_raw(&temp_scratch_value, slot->type, slot->data, slot->size);
        return &temp_scratch_value;
    }

    return &setting->memory_value;
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
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!zmk_custom_setting_is_array(setting)) {
            continue;
        }

        if (custom_subsystem_id && custom_subsystem_id[0] != '\0' &&
            strncmp(setting->custom_subsystem_id, custom_subsystem_id,
                    CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) != 0) {
            continue;
        }

        if (strncmp(setting->array_key, key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0 &&
            setting->array_index == index) {
            return setting;
        }
    }

    return NULL;
}

const char *zmk_custom_setting_public_key(const struct zmk_custom_setting *setting) {
    return zmk_custom_setting_is_array(setting) ? setting->array_key : setting->key;
}

bool zmk_custom_setting_is_array(const struct zmk_custom_setting *setting) {
    return setting && setting->array_key != NULL;
}

static bool same_array(const struct zmk_custom_setting *a, const struct zmk_custom_setting *b) {
    return zmk_custom_setting_is_array(a) && zmk_custom_setting_is_array(b) &&
           strncmp(a->custom_subsystem_id, b->custom_subsystem_id,
                   CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN) == 0 &&
           strncmp(a->array_key, b->array_key, CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN) == 0;
}

static int validate_array_size(const struct zmk_custom_setting *setting, uint32_t array_size) {
    if (!zmk_custom_setting_is_array(setting)) {
        return -EINVAL;
    }

    return array_size <= setting->array_max_size ? 0 : -ERANGE;
}

static void set_array_memory_size_locked(const struct zmk_custom_setting *array_element,
                                         uint32_t array_size) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!same_array(setting, array_element)) {
            continue;
        }

        struct zmk_custom_setting *mutable_setting = (struct zmk_custom_setting *)setting;
        mutable_setting->array_size = array_size;
        if (mutable_setting->array_index >= array_size) {
            clear_temporary_locked(mutable_setting);
        }
    }
}

static void set_array_persistent_size_locked(const struct zmk_custom_setting *array_element,
                                             uint32_t array_size) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!same_array(setting, array_element)) {
            continue;
        }

        ((struct zmk_custom_setting *)setting)->persistent_array_size = array_size;
    }
}

uint32_t zmk_custom_setting_array_size(const struct zmk_custom_setting *setting) {
    if (!zmk_custom_setting_is_array(setting)) {
        return 0;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    uint32_t array_size = setting->array_size;
    k_mutex_unlock(&settings_lock);

    return array_size;
}

uint32_t zmk_custom_setting_array_max_size(const struct zmk_custom_setting *setting) {
    return zmk_custom_setting_is_array(setting) ? setting->array_max_size : 0;
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
        copy_value(&setting->memory_value, value);
    }
    k_mutex_unlock(&settings_lock);

    return 0;
}

static int setting_storage_name(const struct zmk_custom_setting *setting, char *name,
                                size_t name_size) {
    int ret = snprintf(name, name_size, SETTINGS_SUBTREE "/%s/%s", setting->custom_subsystem_id,
                       setting->key);
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

static int delete_inactive_array_values_locked(const struct zmk_custom_setting *array_element,
                                               uint32_t array_size) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!same_array(setting, array_element) || setting->array_index < array_size) {
            continue;
        }

        char name[SETTINGS_MAX_NAME_LEN];
        int ret = setting_storage_name(setting, name, sizeof(name));
        if (ret < 0) {
            return ret;
        }

        ret = settings_delete(name);
        if (ret != 0 && ret != -ENOENT) {
            return ret;
        }

        struct zmk_custom_setting *mutable_setting = (struct zmk_custom_setting *)setting;
        mutable_setting->has_persistent_value = false;
        mutable_setting->dirty = true;
    }

    return 0;
}

static int save_array_locked(const struct zmk_custom_setting *array_element) {
    char size_name[SETTINGS_MAX_NAME_LEN];
    int ret = array_size_storage_name(array_element, size_name, sizeof(size_name));
    if (ret < 0) {
        return ret;
    }

    uint32_t array_size = array_element->array_size;
    ret = settings_save_one(size_name, &array_size, sizeof(array_size));
    if (ret < 0) {
        return ret;
    }

    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!same_array(setting, array_element) || setting->array_index >= array_size) {
            continue;
        }

        char name[SETTINGS_MAX_NAME_LEN];
        ret = setting_storage_name(setting, name, sizeof(name));
        if (ret < 0) {
            return ret;
        }

        const void *data;
        size_t len;
        ret = value_to_storage(&setting->memory_value, &data, &len);
        if (ret < 0) {
            return ret;
        }

        ret = settings_save_one(name, data, len);
        if (ret < 0) {
            return ret;
        }

        struct zmk_custom_setting *mutable_setting = (struct zmk_custom_setting *)setting;
        mutable_setting->has_persistent_value = true;
        mutable_setting->dirty = false;
    }

    set_array_persistent_size_locked(array_element, array_size);
    return delete_inactive_array_values_locked(array_element, array_size);
}

static int save_setting_locked(struct zmk_custom_setting *setting) {
    char name[SETTINGS_MAX_NAME_LEN];
    int ret = setting_storage_name(setting, name, sizeof(name));
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_is_array(setting)) {
        return save_array_locked(setting);
    }

    const void *data;
    size_t len;
    ret = value_to_storage(&setting->memory_value, &data, &len);
    if (ret < 0) {
        return ret;
    }

    ret = settings_save_one(name, data, len);
    if (ret < 0) {
        return ret;
    }

    setting->has_persistent_value = true;
    setting->dirty = false;
    return 0;
}

int zmk_custom_setting_read(const struct zmk_custom_setting *setting,
                            struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (zmk_custom_setting_is_array(setting) && setting->array_index >= setting->array_size) {
        k_mutex_unlock(&settings_lock);
        return -ENOENT;
    }

    copy_value(value, effective_value(setting));
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
        copy_value(&setting->memory_value, value);
        setting->dirty = true;
        clear_temporary_locked(setting);
        return 0;
    case ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST:
        copy_value(&setting->memory_value, value);
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

    if (zmk_custom_setting_is_array(setting) && setting->array_index >= setting->array_size) {
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
    uint32_t array_size = setting->array_size;
    uint32_t array_max_size = setting->array_max_size;
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
    uint32_t array_size = setting->array_size;
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

int zmk_custom_setting_discard(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;

    k_mutex_lock(&settings_lock, K_FOREVER);
    if (zmk_custom_setting_is_array(setting)) {
        set_array_memory_size_locked(setting, setting->persistent_array_size);
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
            copy_value(&setting->memory_value, setting->default_value);
        }
    } else {
        copy_value(&setting->memory_value, setting->default_value);
    }
    setting->dirty = false;
    clear_temporary_locked(setting);
    k_mutex_unlock(&settings_lock);

    raise_setting_changed(setting, ZMK_CUSTOM_SETTING_CHANGED_DISCARDED);
    return 0;
}

int zmk_custom_setting_reset(const struct zmk_custom_setting *const_setting) {
    if (!const_setting) {
        return -EINVAL;
    }

    struct zmk_custom_setting *setting = (struct zmk_custom_setting *)const_setting;
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
        copy_value(&setting->memory_value, setting->default_value);
        if (zmk_custom_setting_is_array(setting)) {
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
                set_array_persistent_size_locked(setting, setting->default_array_size);
                set_array_memory_size_locked(setting, setting->default_array_size);
            }
        }
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

static int apply_scope(const char *custom_subsystem_id, const char *key, const char *key_prefix,
                       uint32_t *affected_count,
                       int (*callback)(const struct zmk_custom_setting *)) {
    uint32_t count = 0;
    int first_error = 0;
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!zmk_custom_setting_matches(setting, custom_subsystem_id, key, key_prefix)) {
            continue;
        }

        int ret = callback(setting);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
            continue;
        }

        if (ret == 0) {
            count++;
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
    bool has_unsaved = setting->temporary_active || setting->dirty;
    if (zmk_custom_setting_is_array(setting)) {
        has_unsaved = has_unsaved || setting->array_size != setting->persistent_array_size;
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
    if (zmk_custom_setting_is_array(setting) && setting->array_index >= setting->array_size) {
        k_mutex_unlock(&settings_lock);
        return -ENOENT;
    }

    visitor(effective_value(setting), user_data);
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

int zmk_custom_setting_write_bytes(const struct zmk_custom_setting *setting, const void *data,
                                   size_t size, enum zmk_custom_setting_write_mode mode) {
    if (!setting || (!data && size > 0)) {
        return -EINVAL;
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
        size_t len = MIN(size, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        memcpy(value.string_value, data, len);
        value.string_value[len] = '\0';
        value.size = len;
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

    copy_value(&setting->memory_value, &value);
    setting->has_persistent_value = true;
    setting->dirty = false;
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

    struct zmk_custom_setting *setting =
        (struct zmk_custom_setting *)zmk_custom_setting_find(custom_subsystem_id, next);
    if (!setting) {
        return -ENOENT;
    }

    uint8_t data[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
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

static int custom_settings_init(void) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        copy_value(&setting->memory_value, setting->default_value);
        setting->persistent_array_size = setting->default_array_size;
        setting->array_size = setting->default_array_size;
        setting->has_persistent_value = false;
        setting->dirty = false;
        setting->temp_slot = -1;
        setting->temporary_active = false;
        setting->initialized = true;
    }

    return 0;
}

SYS_INIT(custom_settings_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
