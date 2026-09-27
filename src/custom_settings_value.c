/* SPDX-License-Identifier: MIT */
/* Copies are explicit at the API boundary; engine reads borrow locked storage. */
#include <errno.h>
#include <string.h>
#include "custom_settings_internal.h"

int copy_value(struct zmk_custom_setting_value_view *dest,
               const struct zmk_custom_setting_value_view *src) {
    if (!dest || !src)
        return -EINVAL;
    if (!ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(src->type) &&
        src->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR) {
        *dest = *src;
        return 0;
    }
    bool string = src->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING;
    size_t capacity = dest->size;
    void *buffer = (void *)dest->bytes_value;
    size_t size = src->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR
                      ? sizeof(struct zmk_custom_setting_behavior_value)
                      : src->size;
    if (src->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR &&
        (uintptr_t)buffer % _Alignof(struct zmk_custom_setting_behavior_value))
        return -EINVAL;
    if (size > capacity || (string && size == capacity))
        return -EMSGSIZE;
    if ((size && !src->bytes_value) || ((size || string) && !buffer))
        return -EINVAL;
    if (size)
        memmove(buffer, src->bytes_value, size);
    if (string)
        ((char *)buffer)[size] = '\0';
    *dest = *src;
    dest->bytes_value = buffer;
    dest->size = size;
    return 0;
}

int zmk_custom_setting_view_blob(struct zmk_custom_setting_value_view *value,
                                 enum zmk_custom_setting_value_type type, const void *data,
                                 size_t size) {
    if (!value || !ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(type) || (size && !data))
        return -EINVAL;
    if (size > UINT16_MAX)
        return -EMSGSIZE;
    *value =
        (struct zmk_custom_setting_value_view){.type = type, .size = size, .bytes_value = data};
    return 0;
}
