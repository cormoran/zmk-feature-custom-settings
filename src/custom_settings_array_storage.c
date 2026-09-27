/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/logging/log.h>
#include "custom_settings_internal.h"
#include "custom_settings_array_storage.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static size_t stride(const struct zmk_custom_setting *array) {
    return ZMK_CUSTOM_SETTING_ARRAY_STRIDE(array->value_type);
}

static void *element(const struct zmk_custom_setting *array, uint32_t index) {
    return (uint8_t *)array->array_state->values + index * stride(array);
}

void array_value_read(const struct zmk_custom_setting *array, uint32_t index,
                      struct zmk_custom_setting_value_view *value) {
    *value = (struct zmk_custom_setting_value_view){.type = array->value_type};
    const void *data = element(array, index);
    if (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(array->value_type)) {
        const struct zmk_custom_setting_blob *blob = data;
        value->size = blob->size;
        value->bytes_value = blob->data;
    } else if (array->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR) {
        value->behavior_value = data;
    } else {
        memcpy(&value->int32_value, data, stride(array));
    }
}

int array_value_write(const struct zmk_custom_setting *array, uint32_t index,
                      const struct zmk_custom_setting_value_view *value) {
    void *dest = element(array, index);
    if (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(array->value_type)) {
        size_t size = value->size;
        return blob_write_locked(&zmk_custom_settings_shared_pool, dest, value->bytes_value, size,
                                 array->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING);
    }
    const void *data = array->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR
                           ? (const void *)value->behavior_value
                           : (const void *)&value->int32_value;
    memcpy(dest, data, stride(array));
    return 0;
}

void array_value_default(const struct zmk_custom_setting *array, uint32_t index) {
    void *dest = element(array, index);
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
    if (compat_array_default(array, index)) {
        return;
    }
#endif
    if (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(array->value_type)) {
        struct zmk_custom_setting_slice slice =
            ((const struct zmk_custom_setting_slice *)array->array_state->defaults)[index];
        struct zmk_custom_setting_blob *blob = dest;
        custom_settings_pool_release(&zmk_custom_settings_shared_pool, blob);
        if (slice.size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE || (slice.size && !slice.data)) {
            LOG_ERR("Invalid array default: %s[%u]", array->key, index);
            return;
        }
        blob->data = (uint8_t *)slice.data;
        blob->size = slice.size;
    } else {
        memcpy(dest, (const uint8_t *)array->array_state->defaults + index * stride(array),
               stride(array));
    }
}

/* Swap ownership, not bytes in the pool. Stable nodes remain at their array
 * indices; list links are retargeted before swapping node contents. */
void array_value_swap(const struct zmk_custom_setting *array, uint32_t a, uint32_t b) {
    if (a == b) {
        return;
    }
    if (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(array->value_type)) {
        struct zmk_custom_setting_blob *left = element(array, a);
        struct zmk_custom_setting_blob *right = element(array, b);
        custom_settings_pool_swap(&zmk_custom_settings_shared_pool, left, right);
    } else {
        uint8_t saved[sizeof(struct zmk_custom_setting_behavior_value)];
        memcpy(saved, element(array, a), stride(array));
        memcpy(element(array, a), element(array, b), stride(array));
        memcpy(element(array, b), saved, stride(array));
    }
}
