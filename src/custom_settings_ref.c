/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

#include "custom_settings_internal.h"
#include <cormoran/zmk/custom_settings/ref.h>

bool zmk_custom_setting_ref_equal(const struct zmk_custom_setting_ref *a,
                                  const struct zmk_custom_setting_ref *b) {
    return a->owner == b->owner && a->index == b->index && a->generation == b->generation &&
           a->kind == b->kind;
}

int zmk_custom_setting_ref_capture(const struct zmk_custom_setting *setting,
                                   struct zmk_custom_setting_ref *ref) {
    if (!setting || !ref) {
        return -EINVAL;
    }
    int ret = 0;
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    *ref = (struct zmk_custom_setting_ref){.owner = setting};
    if (zmk_custom_setting_keyspace_of(setting)) {
        const struct zmk_custom_setting_keyspace *owner = setting->_keyspace;
        ret = -ESTALE;
        for (uint32_t i = 0; i < owner->max_entries; ++i) {
            if (&owner->slots[i].setting == setting && owner->slots[i].in_use) {
                *ref = (struct zmk_custom_setting_ref){.owner = owner,
                                                       .index = i,
                                                       .generation = owner->slots[i].generation,
                                                       .kind = ZMK_CUSTOM_SETTING_REF_KEYSPACE};
                ret = 0;
                break;
            }
        }
    } else if (zmk_custom_setting_is_array(setting) &&
               setting->array_index != ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        ret = -ENOENT;
        ZMK_CUSTOM_SETTING_FOREACH(parent) {
            if (zmk_custom_setting_is_array(parent) &&
                parent->array_state == setting->array_state) {
                *ref = (struct zmk_custom_setting_ref){.owner = parent,
                                                       .index = setting->array_index,
                                                       .kind = ZMK_CUSTOM_SETTING_REF_ARRAY};
                ret = 0;
                break;
            }
        }
    }
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}

int zmk_custom_setting_ref_visit(const struct zmk_custom_setting_ref *ref,
                                 zmk_custom_setting_ref_visitor_t visit, void *context) {
    if (!ref || !ref->owner || !visit) {
        return -EINVAL;
    }
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    const struct zmk_custom_setting *setting = ref->owner;
    struct zmk_custom_setting view;
    int ret = 0;
    switch (ref->kind) {
    case ZMK_CUSTOM_SETTING_REF_STATIC:
        break;
    case ZMK_CUSTOM_SETTING_REF_ARRAY:
        if (!zmk_custom_setting_is_array(setting) || ref->index >= setting->array_state->size) {
            ret = -ENOENT;
            break;
        }
        view = *setting;
        view.array_index = ref->index;
        setting = &view;
        break;
    case ZMK_CUSTOM_SETTING_REF_KEYSPACE: {
        const struct zmk_custom_setting_keyspace *owner = ref->owner;
        if (!IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE) || ref->index >= owner->max_entries ||
            !owner->slots[ref->index].in_use ||
            owner->slots[ref->index].generation != ref->generation) {
            ret = -ESTALE;
            break;
        }
        setting = &owner->slots[ref->index].setting;
        break;
    }
    default:
        ret = -EINVAL;
    }
    if (ret == 0) {
        ret = visit(setting, context);
    }
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}

int zmk_custom_setting_ref_find(const char *subsystem, const char *key, uint32_t index,
                                struct zmk_custom_setting_ref *ref) {
    if (!key || !ref) {
        return -EINVAL;
    }
    *ref = (struct zmk_custom_setting_ref){0};
    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    int ret = -ENOENT;
    if (index == ZMK_CUSTOM_SETTING_ARRAY_NONE) {
        const struct zmk_custom_setting *setting = zmk_custom_setting_find(subsystem, key);
        if (setting) {
            ret = zmk_custom_setting_ref_capture(setting, ref);
        }
    } else if (IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)) {
        const struct zmk_custom_setting *array = zmk_custom_setting_find_array(subsystem, key);
        if (array && index < array->array_state->max_size) {
            *ref = (struct zmk_custom_setting_ref){
                .owner = array, .index = index, .kind = ZMK_CUSTOM_SETTING_REF_ARRAY};
            ret = 0;
        }
    }
    k_mutex_unlock(&custom_settings_lock);
    return ret;
}

struct ref_read_context {
    void *buffer;
    size_t capacity;
    size_t *size;
    enum zmk_custom_setting_value_type *type;
};
static int ref_read(const struct zmk_custom_setting *setting, void *context) {
    struct ref_read_context *ctx = context;
    return zmk_custom_setting_read_into(setting, ctx->buffer, ctx->capacity, ctx->size, ctx->type);
}
int zmk_custom_setting_ref_read_into(const struct zmk_custom_setting_ref *ref, void *buffer,
                                     size_t capacity, size_t *size,
                                     enum zmk_custom_setting_value_type *type) {
    struct ref_read_context context = {buffer, capacity, size, type};
    return zmk_custom_setting_ref_visit(ref, ref_read, &context);
}

struct ref_write_context {
    const void *data;
    size_t size;
    enum zmk_custom_setting_write_mode mode;
};
static int ref_write(const struct zmk_custom_setting *setting, void *context) {
    const struct ref_write_context *ctx = context;
    const struct zmk_custom_setting_keyspace *keyspace = zmk_custom_setting_keyspace_of(setting);
    uint8_t type = keyspace ? keyspace->value_type : setting->value_type;
    switch (type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        return zmk_custom_setting_write_bytes(setting, ctx->data, ctx->size, ctx->mode);
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32: {
        int32_t value;
        if (ctx->size != sizeof(value)) {
            return -EMSGSIZE;
        }
        memcpy(&value, ctx->data, sizeof(value));
        return zmk_custom_setting_set_int32(setting, value, ctx->mode);
    }
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        if (ctx->size != sizeof(bool)) {
            return -EMSGSIZE;
        }
        if (*(const uint8_t *)ctx->data > 1) {
            return -EINVAL;
        }
        return zmk_custom_setting_set_bool(setting, *(const uint8_t *)ctx->data != 0, ctx->mode);
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR: {
        struct zmk_custom_setting_behavior_value value;
        if (ctx->size != sizeof(value)) {
            return -EMSGSIZE;
        }
        memcpy(&value, ctx->data, sizeof(value));
        return zmk_custom_setting_set_behavior(setting, value, ctx->mode);
    }
    default:
        return -EINVAL;
    }
}
int zmk_custom_setting_ref_write(const struct zmk_custom_setting_ref *ref, const void *data,
                                 size_t size, enum zmk_custom_setting_write_mode mode) {
    if (size && !data) {
        return -EINVAL;
    }
    struct ref_write_context context = {data, size, mode};
    return zmk_custom_setting_ref_visit(ref, ref_write, &context);
}
