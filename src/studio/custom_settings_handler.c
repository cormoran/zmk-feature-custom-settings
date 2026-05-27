/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/custom_settings.h>
#include <zmk/custom_settings/custom_settings.pb.h>
#include <zmk/studio/core.h>
#include <zmk/studio/custom.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SUBSYSTEM_IDENTIFIER_STRING "zmk__custom_settings"

static struct zmk_rpc_custom_subsystem_meta custom_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-custom-settings/"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zmk__custom_settings, &custom_settings_meta,
                         custom_settings_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zmk__custom_settings, zmk_custom_settings_Response);

static bool is_unlocked(void) {
    return zmk_studio_core_get_lock_state() == ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
}

static bool needs_unlock(enum zmk_custom_setting_permission permission) {
    return permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE && !is_unlocked();
}

static bool source_targets_local(uint32_t source) {
    return source == ZMK_CUSTOM_SETTING_SOURCE_LOCAL || source == ZMK_CUSTOM_SETTING_SOURCE_ALL;
}

static size_t bounded_strlen(const char *str, size_t max_len) {
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        len++;
    }

    return len;
}

static void copy_string(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) {
        return;
    }

    size_t len = MIN(bounded_strlen(src, dest_size - 1), dest_size - 1);
    memcpy(dest, src, len);
    dest[len] = '\0';
}

static void set_error(zmk_custom_settings_Response *resp, const char *message) {
    zmk_custom_settings_ErrorResponse err = zmk_custom_settings_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = zmk_custom_settings_Response_error_tag;
    resp->response_type.error = err;
}

static void set_status(zmk_custom_settings_Response *resp, uint32_t affected_count,
                       const char *message) {
    zmk_custom_settings_StatusResponse status = zmk_custom_settings_StatusResponse_init_zero;
    status.affected_count = affected_count;
    snprintf(status.message, sizeof(status.message), "%s", message);
    resp->which_response_type = zmk_custom_settings_Response_status_tag;
    resp->response_type.status = status;
}

static zmk_custom_settings_SettingValueType
proto_value_type(enum zmk_custom_setting_value_type type) {
    return (zmk_custom_settings_SettingValueType)type;
}

static zmk_custom_settings_SettingConfidentiality
proto_confidentiality(enum zmk_custom_setting_confidentiality confidentiality) {
    return (zmk_custom_settings_SettingConfidentiality)confidentiality;
}

static zmk_custom_settings_SettingPermission
proto_permission(enum zmk_custom_setting_permission permission) {
    return (zmk_custom_settings_SettingPermission)permission;
}

static zmk_custom_settings_SettingNotificationKind
proto_notification_kind(enum zmk_custom_setting_changed_kind kind) {
    switch (kind) {
    case ZMK_CUSTOM_SETTING_CHANGED_VALUE_UPDATED:
        return zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_VALUE_UPDATED;
    case ZMK_CUSTOM_SETTING_CHANGED_SAVED:
        return zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_SAVED;
    case ZMK_CUSTOM_SETTING_CHANGED_DISCARDED:
        return zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_DISCARDED;
    case ZMK_CUSTOM_SETTING_CHANGED_RESET:
        return zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_RESET;
    default:
        return zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_VALUE_UPDATED;
    }
}

static int proto_to_value(const zmk_custom_settings_SettingValue *src,
                          struct zmk_custom_setting_value *dest) {
    *dest = (struct zmk_custom_setting_value){0};

    switch (src->which_value_type) {
    case zmk_custom_settings_SettingValue_bytes_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES;
        dest->size = src->value_type.bytes_value.size;
        if (dest->size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        memcpy(dest->bytes_value, src->value_type.bytes_value.bytes, dest->size);
        return 0;
    case zmk_custom_settings_SettingValue_int32_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
        dest->int32_value = src->value_type.int32_value;
        return 0;
    case zmk_custom_settings_SettingValue_bool_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL;
        dest->bool_value = src->value_type.bool_value;
        return 0;
    case zmk_custom_settings_SettingValue_string_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING;
        dest->size =
            bounded_strlen(src->value_type.string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        copy_string(dest->string_value, sizeof(dest->string_value), src->value_type.string_value);
        return 0;
    default:
        return -EINVAL;
    }
}

static int value_to_proto(const struct zmk_custom_setting_value *src,
                          zmk_custom_settings_SettingValue *dest) {
    *dest = (zmk_custom_settings_SettingValue)zmk_custom_settings_SettingValue_init_zero;

    switch (src->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        dest->which_value_type = zmk_custom_settings_SettingValue_bytes_value_tag;
        dest->value_type.bytes_value.size = src->size;
        if (src->size > sizeof(dest->value_type.bytes_value.bytes)) {
            return -EMSGSIZE;
        }
        memcpy(dest->value_type.bytes_value.bytes, src->bytes_value, src->size);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        dest->which_value_type = zmk_custom_settings_SettingValue_int32_value_tag;
        dest->value_type.int32_value = src->int32_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        dest->which_value_type = zmk_custom_settings_SettingValue_bool_value_tag;
        dest->value_type.bool_value = src->bool_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        dest->which_value_type = zmk_custom_settings_SettingValue_string_value_tag;
        copy_string(dest->value_type.string_value, sizeof(dest->value_type.string_value),
                    src->string_value);
        return 0;
    default:
        return -EINVAL;
    }
}

static int constraint_to_proto(const struct zmk_custom_setting_constraint *src,
                               zmk_custom_settings_SettingConstraint *dest) {
    *dest = (zmk_custom_settings_SettingConstraint)zmk_custom_settings_SettingConstraint_init_zero;

    switch (src->type) {
    case ZMK_CUSTOM_SETTING_CONSTRAINT_NONE:
        return -ENOENT;
    case ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE: {
        dest->which_constraint_type = zmk_custom_settings_SettingConstraint_range_tag;
        dest->constraint_type.range.has_min = true;
        dest->constraint_type.range.has_max = true;
        int ret = value_to_proto(&src->range.min, &dest->constraint_type.range.min);
        if (ret < 0) {
            return ret;
        }
        return value_to_proto(&src->range.max, &dest->constraint_type.range.max);
    }
    case ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS:
        dest->which_constraint_type = zmk_custom_settings_SettingConstraint_options_tag;
        dest->constraint_type.options.values_count =
            MIN(src->options.count, ARRAY_SIZE(dest->constraint_type.options.values));
        dest->constraint_type.options.labels_count =
            MIN(src->options.count, ARRAY_SIZE(dest->constraint_type.options.labels));
        for (size_t i = 0; i < dest->constraint_type.options.values_count; i++) {
            int ret =
                value_to_proto(&src->options.values[i], &dest->constraint_type.options.values[i]);
            if (ret < 0) {
                return ret;
            }
            if (src->options.labels) {
                copy_string(dest->constraint_type.options.labels[i],
                            sizeof(dest->constraint_type.options.labels[i]),
                            src->options.labels[i]);
            }
        }
        return 0;
    case ZMK_CUSTOM_SETTING_CONSTRAINT_HID_USAGE:
        dest->which_constraint_type = zmk_custom_settings_SettingConstraint_hid_usage_tag;
        dest->constraint_type.hid_usage.usage_page = src->hid_usage.usage_page;
        dest->constraint_type.hid_usage.usage_min = src->hid_usage.usage_min;
        dest->constraint_type.hid_usage.usage_max = src->hid_usage.usage_max;
        return 0;
    case ZMK_CUSTOM_SETTING_CONSTRAINT_LAYER_ID:
        dest->which_constraint_type = zmk_custom_settings_SettingConstraint_layer_id_tag;
        return 0;
    case ZMK_CUSTOM_SETTING_CONSTRAINT_BEHAVIOR_ID:
        dest->which_constraint_type = zmk_custom_settings_SettingConstraint_behavior_id_tag;
        return 0;
    default:
        return -EINVAL;
    }
}

static int setting_to_proto(const struct zmk_custom_setting *setting,
                            zmk_custom_settings_Setting *dest, bool include_value,
                            uint32_t source) {
    *dest = (zmk_custom_settings_Setting)zmk_custom_settings_Setting_init_zero;

    copy_string(dest->subsystem_id, sizeof(dest->subsystem_id), setting->subsystem_id);
    copy_string(dest->key, sizeof(dest->key), setting->key);
    dest->value_type = proto_value_type(setting->value_type);
    dest->confidentiality = proto_confidentiality(setting->confidentiality);
    dest->read_permission = proto_permission(setting->read_permission);
    dest->write_permission = proto_permission(setting->write_permission);
    dest->has_unsaved_value = zmk_custom_setting_has_unsaved_value(setting);
    dest->source = source;

    if (setting->constraint.type != ZMK_CUSTOM_SETTING_CONSTRAINT_NONE) {
        dest->has_constraint = true;
        int ret = constraint_to_proto(&setting->constraint, &dest->constraint);
        if (ret < 0) {
            dest->has_constraint = false;
        }
    }

    if (include_value &&
        setting->confidentiality != ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE) {
        struct zmk_custom_setting_value value;
        int ret = zmk_custom_setting_read(setting, &value);
        if (ret < 0) {
            return ret;
        }

        dest->has_value = true;
        ret = value_to_proto(&value, &dest->value);
        if (ret < 0) {
            dest->has_value = false;
            return ret;
        }
    }

    return 0;
}

static int custom_subsystem_index(void) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        if (strcmp(custom_subsys->identifier, SUBSYSTEM_IDENTIFIER_STRING) == 0) {
            return i;
        }
    }

    return -ENOENT;
}

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const zmk_custom_settings_Notification *notification =
        (const zmk_custom_settings_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, zmk_custom_settings_Notification_fields, notification);
}

static int raise_setting_notification(const struct zmk_custom_setting *setting,
                                      zmk_custom_settings_SettingNotificationKind kind,
                                      bool include_value, uint32_t source) {
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }

    zmk_custom_settings_Notification notification = zmk_custom_settings_Notification_init_zero;
    notification.kind = kind;
    notification.has_setting = true;
    int ret = setting_to_proto(setting, &notification.setting, include_value, source);
    if (ret < 0) {
        return ret;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_notification_payload,
        .arg = &notification,
    };

    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
}

static bool can_include_value(const struct zmk_custom_setting *setting) {
    return setting->confidentiality != ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE &&
           !needs_unlock(setting->read_permission);
}

static int handle_list_settings(const zmk_custom_settings_ListSettingsRequest *req,
                                zmk_custom_settings_Response *resp) {
    const zmk_custom_settings_SettingScope *scope = &req->scope;
    uint32_t count = 0;

    if (!source_targets_local(scope->source)) {
        set_status(resp, 0, "No local settings matched source");
        return 0;
    }

    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!zmk_custom_setting_matches(setting, scope->subsystem_id, scope->key,
                                        scope->key_prefix)) {
            continue;
        }

        int ret = raise_setting_notification(
            setting,
            zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_LIST_ITEM,
            can_include_value(setting), ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
        if (ret < 0) {
            return ret;
        }
        count++;
    }

    set_status(resp, count, "List started");
    return 0;
}

static int handle_get_setting(const zmk_custom_settings_GetSettingRequest *req,
                              zmk_custom_settings_Response *resp) {
    const zmk_custom_settings_SettingRef *ref = &req->setting;
    if (!source_targets_local(ref->source)) {
        return -ENOENT;
    }

    const struct zmk_custom_setting *setting = zmk_custom_setting_find(ref->subsystem_id, ref->key);
    if (!setting) {
        return -ENOENT;
    }

    if (needs_unlock(setting->read_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    zmk_custom_settings_GetSettingResponse get = zmk_custom_settings_GetSettingResponse_init_zero;
    get.has_setting = true;
    int ret = setting_to_proto(setting, &get.setting, true, ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }

    resp->which_response_type = zmk_custom_settings_Response_get_setting_tag;
    resp->response_type.get_setting = get;
    return 0;
}

static int handle_write_setting(const zmk_custom_settings_WriteSettingRequest *req,
                                zmk_custom_settings_Response *resp) {
    const zmk_custom_settings_SettingRef *ref = &req->setting;
    if (!source_targets_local(ref->source)) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    const struct zmk_custom_setting *setting = zmk_custom_setting_find(ref->subsystem_id, ref->key);
    if (!setting) {
        return -ENOENT;
    }

    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    struct zmk_custom_setting_value value;
    int ret = proto_to_value(&req->value, &value);
    if (ret < 0) {
        return ret;
    }

    enum zmk_custom_setting_write_mode mode =
        req->mode == zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    ret = zmk_custom_setting_write(setting, &value, mode);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Setting written");
    return 0;
}

static bool scope_write_unlock_required(const zmk_custom_settings_SettingScope *scope) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (zmk_custom_setting_matches(setting, scope->subsystem_id, scope->key,
                                       scope->key_prefix) &&
            needs_unlock(setting->write_permission)) {
            return true;
        }
    }

    return false;
}

static int handle_scope_mutation(const zmk_custom_settings_SettingScope *scope,
                                 zmk_custom_settings_Response *resp, const char *message,
                                 int (*callback)(const char *, const char *, const char *,
                                                 uint32_t *)) {
    if (!source_targets_local(scope->source)) {
        set_status(resp, 0, "No local settings matched source");
        return 0;
    }

    if (scope_write_unlock_required(scope)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    uint32_t count = 0;
    int ret = callback(scope->subsystem_id, scope->key, scope->key_prefix, &count);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, count, message);
    return 0;
}

static bool custom_settings_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                               pb_callback_t *encode_response) {
    zmk_custom_settings_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(zmk__custom_settings, encode_response);

    zmk_custom_settings_Request req = zmk_custom_settings_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, zmk_custom_settings_Request_fields, &req)) {
        LOG_WRN("Failed to decode custom settings request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    int ret = 0;
    switch (req.which_request_type) {
    case zmk_custom_settings_Request_list_settings_tag:
        ret = handle_list_settings(&req.request_type.list_settings, resp);
        break;
    case zmk_custom_settings_Request_get_setting_tag:
        ret = handle_get_setting(&req.request_type.get_setting, resp);
        break;
    case zmk_custom_settings_Request_write_setting_tag:
        ret = handle_write_setting(&req.request_type.write_setting, resp);
        break;
    case zmk_custom_settings_Request_save_settings_tag:
        ret = handle_scope_mutation(&req.request_type.save_settings.scope, resp, "Settings saved",
                                    zmk_custom_settings_save_scope);
        break;
    case zmk_custom_settings_Request_discard_settings_tag:
        ret = handle_scope_mutation(&req.request_type.discard_settings.scope, resp,
                                    "Settings discarded", zmk_custom_settings_discard_scope);
        break;
    case zmk_custom_settings_Request_reset_settings_tag:
        ret = handle_scope_mutation(&req.request_type.reset_settings.scope, resp, "Settings reset",
                                    zmk_custom_settings_reset_scope);
        break;
    default:
        ret = -ENOTSUP;
        break;
    }

    if (ret < 0) {
        LOG_WRN("Custom settings request failed: %d", ret);
        switch (ret) {
        case -ENOENT:
            set_error(resp, "Setting not found");
            break;
        case -EINVAL:
            set_error(resp, "Invalid request");
            break;
        case -ERANGE:
            set_error(resp, "Value out of range");
            break;
        default:
            set_error(resp, "Failed to process request");
            break;
        }
    }

    return true;
}

static int setting_changed_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *ev = as_zmk_custom_setting_changed(eh);
    if (!ev || !ev->setting) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    raise_setting_notification(ev->setting, proto_notification_kind(ev->kind),
                               can_include_value(ev->setting), ev->source);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_settings_studio, setting_changed_listener);
ZMK_SUBSCRIPTION(custom_settings_studio, zmk_custom_setting_changed);
