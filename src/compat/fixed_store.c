/* SPDX-License-Identifier: MIT */
#include <string.h>
#include "../custom_settings_internal.h"

int compat_blob_store_set_raw(const struct zmk_custom_setting *setting, const void *data,
                              size_t size) {
    if (size > 0) {
        memmove(setting->state->blob.data, data, size);
    }
    if (setting->value_type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING &&
        setting->state->blob.data != NULL) {
        setting->state->blob.data[size] = '\0';
    }
    setting->state->blob.size = size;
    return 0;
}
