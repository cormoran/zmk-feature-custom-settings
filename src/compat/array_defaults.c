/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/logging/log.h>
#include "../custom_settings_internal.h"
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Interpret old ROM carriers only. Live elements are always typed. */
bool compat_array_default(const struct zmk_custom_setting *array, uint32_t index) {
    if (!array->array_state->defaults_are_carriers) {
        return false;
    }
    const struct zmk_custom_setting_value *value =
        &((const struct zmk_custom_setting_value *)array->array_state->defaults)[index];
    size_t stride = ZMK_CUSTOM_SETTING_ARRAY_STRIDE(array->value_type);
    void *dest = (uint8_t *)array->array_state->values + index * stride;
    if (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(array->value_type)) {
        struct zmk_custom_setting_blob *blob = dest;
        size_t size = array->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING
                          ? bounded_strlen(value->string_value, sizeof(value->string_value))
                          : value->size;
        custom_settings_pool_release(&zmk_custom_settings_shared_pool, blob);
        if (size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE || value->type != array->value_type) {
            LOG_ERR("Invalid legacy array default: %s[%u]", array->key, index);
            return true;
        }
        blob->data = (uint8_t *)value->bytes_value;
        blob->size = size;
    } else {
        memcpy(dest, &value->int32_value, stride);
    }
    return true;
}
