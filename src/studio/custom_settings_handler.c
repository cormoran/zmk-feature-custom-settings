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
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

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

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
struct zmk_custom_settings_relay_request {
    uint8_t source;
    uint8_t payload_size;
    uint8_t payload[CONFIG_ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE];
};

struct zmk_custom_settings_relay_notification {
    uint8_t source;
    uint8_t payload_size;
    uint8_t payload[CONFIG_ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE];
};

BUILD_ASSERT(sizeof(struct zmk_custom_settings_relay_request) <=
                 CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for custom settings relay "
             "requests");
BUILD_ASSERT(sizeof(struct zmk_custom_settings_relay_notification) <=
                 CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN,
             "CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN is too small for custom settings relay "
             "notifications");

ZMK_EVENT_DECLARE(zmk_custom_settings_relay_request);
ZMK_EVENT_DECLARE(zmk_custom_settings_relay_notification);
ZMK_EVENT_IMPL(zmk_custom_settings_relay_request);
ZMK_EVENT_IMPL(zmk_custom_settings_relay_notification);

ZMK_RELAY_EVENT_HANDLE(zmk_custom_settings_relay_request, csr, source);
ZMK_RELAY_EVENT_HANDLE(zmk_custom_settings_relay_notification, csn, source);
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_custom_settings_relay_request, csr, source);
ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_custom_settings_relay_notification, csn, source);
#endif

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

static int scalar_proto_to_value(const zmk_custom_settings_SettingScalarValue *src,
                                 struct zmk_custom_setting_value *dest) {
    *dest = (struct zmk_custom_setting_value){0};

    switch (src->which_value_type) {
    case zmk_custom_settings_SettingScalarValue_bytes_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES;
        dest->size = src->value_type.bytes_value.size;
        if (dest->size > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
            return -EMSGSIZE;
        }
        memcpy(dest->bytes_value, src->value_type.bytes_value.bytes, dest->size);
        return 0;
    case zmk_custom_settings_SettingScalarValue_int32_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
        dest->int32_value = src->value_type.int32_value;
        return 0;
    case zmk_custom_settings_SettingScalarValue_bool_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL;
        dest->bool_value = src->value_type.bool_value;
        return 0;
    case zmk_custom_settings_SettingScalarValue_string_value_tag:
        dest->type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING;
        dest->size =
            bounded_strlen(src->value_type.string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        copy_string(dest->string_value, sizeof(dest->string_value), src->value_type.string_value);
        return 0;
    default:
        return -EINVAL;
    }
}

static int proto_to_value(const zmk_custom_settings_SettingValue *src,
                          struct zmk_custom_setting_value *dest, bool *is_array,
                          uint32_t *array_index, uint32_t *array_size) {
    *is_array = false;

    switch (src->which_value_type) {
    case zmk_custom_settings_SettingValue_bytes_value_tag:
    case zmk_custom_settings_SettingValue_int32_value_tag:
    case zmk_custom_settings_SettingValue_bool_value_tag:
    case zmk_custom_settings_SettingValue_string_value_tag: {
        zmk_custom_settings_SettingScalarValue scalar =
            zmk_custom_settings_SettingScalarValue_init_zero;
        switch (src->which_value_type) {
        case zmk_custom_settings_SettingValue_bytes_value_tag:
            scalar.which_value_type = zmk_custom_settings_SettingScalarValue_bytes_value_tag;
            scalar.value_type.bytes_value.size = src->value_type.bytes_value.size;
            memcpy(scalar.value_type.bytes_value.bytes, src->value_type.bytes_value.bytes,
                   scalar.value_type.bytes_value.size);
            break;
        case zmk_custom_settings_SettingValue_int32_value_tag:
            scalar.which_value_type = zmk_custom_settings_SettingScalarValue_int32_value_tag;
            scalar.value_type.int32_value = src->value_type.int32_value;
            break;
        case zmk_custom_settings_SettingValue_bool_value_tag:
            scalar.which_value_type = zmk_custom_settings_SettingScalarValue_bool_value_tag;
            scalar.value_type.bool_value = src->value_type.bool_value;
            break;
        case zmk_custom_settings_SettingValue_string_value_tag:
            scalar.which_value_type = zmk_custom_settings_SettingScalarValue_string_value_tag;
            copy_string(scalar.value_type.string_value, sizeof(scalar.value_type.string_value),
                        src->value_type.string_value);
            break;
        default:
            return -EINVAL;
        }
        return scalar_proto_to_value(&scalar, dest);
    }
    case zmk_custom_settings_SettingValue_array_value_tag:
        if (!src->value_type.array_value.has_value) {
            return -EINVAL;
        }
        *is_array = true;
        *array_index = src->value_type.array_value.index;
        *array_size = src->value_type.array_value.size;
        return scalar_proto_to_value(&src->value_type.array_value.value, dest);
    default:
        return -EINVAL;
    }
}

static int value_to_scalar_proto(const struct zmk_custom_setting_value *src,
                                 zmk_custom_settings_SettingScalarValue *dest) {
    *dest =
        (zmk_custom_settings_SettingScalarValue)zmk_custom_settings_SettingScalarValue_init_zero;

    switch (src->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        dest->which_value_type = zmk_custom_settings_SettingScalarValue_bytes_value_tag;
        dest->value_type.bytes_value.size = src->size;
        if (src->size > sizeof(dest->value_type.bytes_value.bytes)) {
            return -EMSGSIZE;
        }
        memcpy(dest->value_type.bytes_value.bytes, src->bytes_value, src->size);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        dest->which_value_type = zmk_custom_settings_SettingScalarValue_int32_value_tag;
        dest->value_type.int32_value = src->int32_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        dest->which_value_type = zmk_custom_settings_SettingScalarValue_bool_value_tag;
        dest->value_type.bool_value = src->bool_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        dest->which_value_type = zmk_custom_settings_SettingScalarValue_string_value_tag;
        copy_string(dest->value_type.string_value, sizeof(dest->value_type.string_value),
                    src->string_value);
        return 0;
    default:
        return -EINVAL;
    }
}

static int value_to_proto(const struct zmk_custom_setting *setting,
                          const struct zmk_custom_setting_value *src,
                          zmk_custom_settings_SettingValue *dest) {
    *dest = (zmk_custom_settings_SettingValue)zmk_custom_settings_SettingValue_init_zero;

    if (zmk_custom_setting_is_array(setting)) {
        dest->which_value_type = zmk_custom_settings_SettingValue_array_value_tag;
        dest->value_type.array_value.index = setting->array_index;
        dest->value_type.array_value.size = setting->array_size;
        dest->value_type.array_value.has_value = true;
        return value_to_scalar_proto(src, &dest->value_type.array_value.value);
    }

    zmk_custom_settings_SettingScalarValue scalar =
        zmk_custom_settings_SettingScalarValue_init_zero;
    int ret = value_to_scalar_proto(src, &scalar);
    if (ret < 0) {
        return ret;
    }

    switch (scalar.which_value_type) {
    case zmk_custom_settings_SettingScalarValue_bytes_value_tag:
        dest->which_value_type = zmk_custom_settings_SettingValue_bytes_value_tag;
        dest->value_type.bytes_value.size = scalar.value_type.bytes_value.size;
        memcpy(dest->value_type.bytes_value.bytes, scalar.value_type.bytes_value.bytes,
               dest->value_type.bytes_value.size);
        return 0;
    case zmk_custom_settings_SettingScalarValue_int32_value_tag:
        dest->which_value_type = zmk_custom_settings_SettingValue_int32_value_tag;
        dest->value_type.int32_value = scalar.value_type.int32_value;
        return 0;
    case zmk_custom_settings_SettingScalarValue_bool_value_tag:
        dest->which_value_type = zmk_custom_settings_SettingValue_bool_value_tag;
        dest->value_type.bool_value = scalar.value_type.bool_value;
        return 0;
    case zmk_custom_settings_SettingScalarValue_string_value_tag:
        dest->which_value_type = zmk_custom_settings_SettingValue_string_value_tag;
        copy_string(dest->value_type.string_value, sizeof(dest->value_type.string_value),
                    scalar.value_type.string_value);
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
        int ret = value_to_scalar_proto(&src->range.min, &dest->constraint_type.range.min);
        if (ret < 0) {
            return ret;
        }
        return value_to_scalar_proto(&src->range.max, &dest->constraint_type.range.max);
    }
    case ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS:
        dest->which_constraint_type = zmk_custom_settings_SettingConstraint_options_tag;
        dest->constraint_type.options.values_count =
            MIN(src->options.count, ARRAY_SIZE(dest->constraint_type.options.values));
        dest->constraint_type.options.labels_count =
            MIN(src->options.count, ARRAY_SIZE(dest->constraint_type.options.labels));
        for (size_t i = 0; i < dest->constraint_type.options.values_count; i++) {
            int ret = value_to_scalar_proto(&src->options.values[i],
                                            &dest->constraint_type.options.values[i]);
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

    copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id),
                setting->custom_subsystem_id);
    copy_string(dest->key, sizeof(dest->key), zmk_custom_setting_public_key(setting));
    dest->value_type = proto_value_type(setting->value_type);
    dest->confidentiality = proto_confidentiality(setting->confidentiality);
    dest->read_permission = proto_permission(setting->read_permission);
    dest->write_permission = proto_permission(setting->write_permission);
    dest->has_unsaved_value = zmk_custom_setting_has_unsaved_value(setting);
    dest->source = source;
    dest->is_array = zmk_custom_setting_is_array(setting);
    if (dest->is_array) {
        dest->array_index = setting->array_index;
        dest->array_size = setting->array_size;
    }

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
        ret = value_to_proto(setting, &value, &dest->value);
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

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
static bool encode_raw_payload(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct zmk_custom_settings_relay_notification *notification =
        (const struct zmk_custom_settings_relay_notification *)*arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, notification->payload, notification->payload_size);
}

static int raise_encoded_studio_notification(
    const struct zmk_custom_settings_relay_notification *notification) {
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_raw_payload,
        .arg = (void *)notification,
    };

    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
}
#endif

static int raise_setting_notification(const struct zmk_custom_setting *setting,
                                      zmk_custom_settings_SettingNotificationKind kind,
                                      bool include_value, uint32_t source) {
    zmk_custom_settings_Notification notification = zmk_custom_settings_Notification_init_zero;
    notification.which_notification_type = zmk_custom_settings_Notification_setting_tag;
    notification.notification_type.setting.kind = kind;
    notification.notification_type.setting.has_setting = true;
    int ret = setting_to_proto(setting, &notification.notification_type.setting.setting,
                               include_value, source);
    if (ret < 0) {
        return ret;
    }

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    struct zmk_custom_settings_relay_notification relay_notification = {
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    };
    pb_ostream_t stream =
        pb_ostream_from_buffer(relay_notification.payload, sizeof(relay_notification.payload));
    if (!pb_encode(&stream, zmk_custom_settings_Notification_fields, &notification)) {
        LOG_WRN("Failed to encode custom settings relay notification: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }
    relay_notification.payload_size = stream.bytes_written;
    return raise_zmk_custom_settings_relay_notification(relay_notification);
#else
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_notification_payload,
        .arg = &notification,
    };

    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = (uint8_t)index,
        .encode_payload = payload,
    });
#endif
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
        if (!zmk_custom_setting_matches(setting, scope->custom_subsystem_id, scope->key,
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

    const struct zmk_custom_setting *setting =
        ref->has_array_index ? zmk_custom_setting_find_array_element(ref->custom_subsystem_id,
                                                                     ref->key, ref->array_index)
                             : zmk_custom_setting_find(ref->custom_subsystem_id, ref->key);
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

    struct zmk_custom_setting_value value;
    bool value_is_array = false;
    uint32_t array_index = 0;
    uint32_t array_size = 0;
    int ret = proto_to_value(&req->value, &value, &value_is_array, &array_index, &array_size);
    if (ret < 0) {
        return ret;
    }

    const struct zmk_custom_setting *setting = NULL;
    if (value_is_array || ref->has_array_index) {
        uint32_t resolved_index = value_is_array ? array_index : ref->array_index;
        if (value_is_array && ref->has_array_index && ref->array_index != array_index) {
            return -EINVAL;
        }

        setting = zmk_custom_setting_find_array_element(ref->custom_subsystem_id, ref->key,
                                                        resolved_index);
        if (!setting) {
            return -ENOENT;
        }
        if (value_is_array && array_size != setting->array_size) {
            return -EINVAL;
        }
    } else {
        setting = zmk_custom_setting_find(ref->custom_subsystem_id, ref->key);
        if (!setting) {
            return -ENOENT;
        }
    }

    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
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
        if (zmk_custom_setting_matches(setting, scope->custom_subsystem_id, scope->key,
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
    int ret = callback(scope->custom_subsystem_id, scope->key, scope->key_prefix, &count);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, count, message);
    return 0;
}

static bool should_relay_to_peripherals(const zmk_custom_settings_Request *req) {
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    switch (req->which_request_type) {
    case zmk_custom_settings_Request_list_settings_tag:
        return req->request_type.list_settings.scope.source == ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_save_settings_tag:
        return req->request_type.save_settings.scope.source == ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_discard_settings_tag:
        return req->request_type.discard_settings.scope.source == ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_reset_settings_tag:
        return req->request_type.reset_settings.scope.source == ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_write_setting_tag:
        return req->request_type.write_setting.setting.source == ZMK_CUSTOM_SETTING_SOURCE_ALL;
    default:
        return false;
    }
#else
    return false;
#endif
}

static int relay_request_to_peripherals(const zmk_custom_CallRequest *raw_request) {
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (raw_request->payload.size > CONFIG_ZMK_CUSTOM_SETTINGS_RELAY_PAYLOAD_MAX_SIZE) {
        return -EMSGSIZE;
    }

    struct zmk_custom_settings_relay_request relay_request = {
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    };
    relay_request.payload_size = raw_request->payload.size;
    memcpy(relay_request.payload, raw_request->payload.bytes, raw_request->payload.size);
    return raise_zmk_custom_settings_relay_request(relay_request);
#else
    return 0;
#endif
}

static int process_request(const zmk_custom_settings_Request *req,
                           zmk_custom_settings_Response *resp) {
    switch (req->which_request_type) {
    case zmk_custom_settings_Request_list_settings_tag:
        return handle_list_settings(&req->request_type.list_settings, resp);
    case zmk_custom_settings_Request_get_setting_tag:
        return handle_get_setting(&req->request_type.get_setting, resp);
    case zmk_custom_settings_Request_write_setting_tag:
        return handle_write_setting(&req->request_type.write_setting, resp);
    case zmk_custom_settings_Request_save_settings_tag:
        return handle_scope_mutation(&req->request_type.save_settings.scope, resp, "Settings saved",
                                     zmk_custom_settings_save_scope);
    case zmk_custom_settings_Request_discard_settings_tag:
        return handle_scope_mutation(&req->request_type.discard_settings.scope, resp,
                                     "Settings discarded", zmk_custom_settings_discard_scope);
    case zmk_custom_settings_Request_reset_settings_tag:
        return handle_scope_mutation(&req->request_type.reset_settings.scope, resp,
                                     "Settings reset", zmk_custom_settings_reset_scope);
    default:
        return -ENOTSUP;
    }
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

    int ret = process_request(&req, resp);
    if (ret == 0 && should_relay_to_peripherals(&req)) {
        ret = relay_request_to_peripherals(raw_request);
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

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
static int relay_request_listener(const zmk_event_t *eh) {
    const struct zmk_custom_settings_relay_request *ev = as_zmk_custom_settings_relay_request(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_custom_settings_Request req = zmk_custom_settings_Request_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(ev->payload, ev->payload_size);
    if (!pb_decode(&stream, zmk_custom_settings_Request_fields, &req)) {
        LOG_WRN("Failed to decode relayed custom settings request: %s", PB_GET_ERROR(&stream));
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_custom_settings_Response resp = zmk_custom_settings_Response_init_zero;
    int ret = process_request(&req, &resp);
    if (ret < 0) {
        LOG_WRN("Relayed custom settings request failed: %d", ret);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int relay_notification_listener(const zmk_event_t *eh) {
    const struct zmk_custom_settings_relay_notification *ev =
        as_zmk_custom_settings_relay_notification(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    raise_encoded_studio_notification(ev);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_settings_relay_request, relay_request_listener);
ZMK_SUBSCRIPTION(custom_settings_relay_request, zmk_custom_settings_relay_request);
ZMK_LISTENER(custom_settings_relay_notification, relay_notification_listener);
ZMK_SUBSCRIPTION(custom_settings_relay_notification, zmk_custom_settings_relay_notification);
#endif
