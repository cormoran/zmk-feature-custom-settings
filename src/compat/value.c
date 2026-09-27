/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>
#include "../custom_settings_internal.h"

struct zmk_custom_setting_value_view value_borrow(const struct zmk_custom_setting_value *value) {
    if (value->type < ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
        value->type > ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR ||
        (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(value->type) && value->size > UINT16_MAX))
        return (struct zmk_custom_setting_value_view){0};
    struct zmk_custom_setting_value_view view = {
        .type = value->type,
        .size = ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(value->type) ? value->size : 0};
    switch (value->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        view.bytes_value = value->bytes_value;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        view.string_value = value->string_value;
        view.size = bounded_strlen(value->string_value, sizeof(value->string_value));
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR:
        view.behavior_value = &value->behavior_value;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        view.int32_value = value->int32_value;
        break;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        view.bool_value = value->bool_value;
        break;
    default:
        view.type = 0;
        break;
    }
    return view;
}
struct zmk_custom_setting_value_view value_output(struct zmk_custom_setting_value *value) {
    return ZMK_CUSTOM_SETTING_VIEW_BUFFER(
        value->bytes_value, MAX(sizeof(value->string_value), sizeof(value->behavior_value)));
}
int value_finish(struct zmk_custom_setting_value *value,
                 const struct zmk_custom_setting_value_view *view) {
    if (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(view->type)) {
        if (view->size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE)
            return -EMSGSIZE;
    } else if (view->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR) {
        value->behavior_value = *view->behavior_value;
    } else if (view->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
        value->int32_value = view->int32_value;
    } else if (view->type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        value->bool_value = view->bool_value;
    }
    value->type = view->type;
    value->size = view->size;
    return 0;
}

int value_check_input(const struct zmk_custom_setting_value *value) {
    if (ZMK_CUSTOM_SETTING_TYPE_IS_BLOB(value->type) &&
        value->size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE)
        return -EMSGSIZE;
    return 0;
}

int value_visit(const struct zmk_custom_setting_value_view *view,
                zmk_custom_setting_value_visitor_t visitor, void *user_data) {
    struct zmk_custom_setting_value value;
    struct zmk_custom_setting_value_view output = value_output(&value);
    int ret = copy_value(&output, view);
    if (!ret)
        ret = value_finish(&value, &output);
    if (!ret)
        visitor(&value, user_data);
    return ret;
}
