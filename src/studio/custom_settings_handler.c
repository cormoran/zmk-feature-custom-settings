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

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/custom_settings.h>
#include <zmk/custom_settings/custom_settings.pb.h>
#include <zmk/studio/core.h>
#include <zmk/workqueue.h>
#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#define ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC 1
#else
#define ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC 0
#endif
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SUBSYSTEM_IDENTIFIER_STRING "zmk__custom_settings"
#define LIST_SETTINGS_NOTIFICATION_DELAY K_MSEC(10)
#define LIST_SETTINGS_RELAY_DELAY K_MSEC(20)

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static struct zmk_rpc_custom_subsystem_meta custom_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-custom-settings/"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(zmk__custom_settings, &custom_settings_meta,
                         custom_settings_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(zmk__custom_settings, zmk_custom_settings_Response);
#endif

static bool studio_is_unlocked(void) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    return zmk_studio_core_get_lock_state() == ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
#else
    return true;
#endif
}

static bool needs_unlock(enum zmk_custom_setting_permission permission) {
    return permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE && !studio_is_unlocked();
}

static bool source_targets_local(uint32_t source) {
    return source == ZMK_CUSTOM_SETTING_SOURCE_LOCAL || source == ZMK_CUSTOM_SETTING_SOURCE_ALL;
}

struct zmk_custom_settings_setting_ref {
    bool has_custom_subsystem_id;
    char custom_subsystem_id[CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN];
    bool has_key;
    char key[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    bool has_source;
    uint32_t source;
    bool has_array_index;
    uint32_t array_index;
};

struct zmk_custom_settings_setting_scope {
    bool has_custom_subsystem_id;
    char custom_subsystem_id[CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN];
    bool has_key;
    char key[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    bool has_key_prefix;
    char key_prefix[CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN];
    bool has_source;
    uint32_t source;
};

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
enum zmk_custom_settings_relay_request_type {
    ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_NONE,
    ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_LIST,
    ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_WRITE,
    ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_SAVE,
    ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_DISCARD,
    ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_RESET,
    ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_PUSH_BACK,
    ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_POP_BACK,
};

struct zmk_custom_settings_relay_value {
    uint8_t type;
    uint8_t size;
    union {
        uint8_t bytes_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
        int32_t int32_value;
        bool bool_value;
        char string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE + 1];
    };
};

struct zmk_custom_settings_relay_request {
    uint8_t source;
    uint8_t type;
    union {
        struct {
            bool require_meta;
            struct zmk_custom_settings_setting_scope scope;
        } list_settings;
        struct {
            bool value_is_array;
            uint8_t mode;
            uint32_t array_index;
            uint32_t array_size;
            struct zmk_custom_settings_setting_ref setting;
            struct zmk_custom_settings_relay_value value;
        } write_setting;
        struct {
            uint8_t mode;
            struct zmk_custom_settings_setting_ref setting;
            struct zmk_custom_settings_relay_value value;
        } push_back_array;
        struct {
            uint8_t mode;
            struct zmk_custom_settings_setting_ref setting;
        } pop_back_array;
        struct {
            struct zmk_custom_settings_setting_scope scope;
        } scope_mutation;
    } request;
};

struct zmk_custom_settings_relay_notification {
    uint8_t source;
    char custom_subsystem_id[CONFIG_ZMK_CUSTOM_SETTINGS_CUSTOM_SUBSYSTEM_ID_MAX_LEN];
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

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static uint32_t ref_source(const zmk_custom_settings_SettingRef *ref) {
    return ref->has_source ? ref->source : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}
#endif

static uint32_t setting_ref_source(const struct zmk_custom_settings_setting_ref *ref) {
    return ref->has_source ? ref->source : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static uint32_t scope_source(const zmk_custom_settings_SettingScope *scope) {
    return scope->has_source ? scope->source : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}
#endif

static uint32_t setting_scope_source(const struct zmk_custom_settings_setting_scope *scope) {
    return scope->has_source ? scope->source : ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
}

static const char *
setting_ref_custom_subsystem_id(const struct zmk_custom_settings_setting_ref *ref) {
    return ref->has_custom_subsystem_id ? ref->custom_subsystem_id : NULL;
}

static const char *setting_ref_key(const struct zmk_custom_settings_setting_ref *ref) {
    return ref->has_key ? ref->key : NULL;
}

static const struct zmk_custom_setting *
setting_for_ref(const struct zmk_custom_settings_setting_ref *ref) {
    if (!ref->has_key) {
        return NULL;
    }

    return ref->has_array_index
               ? zmk_custom_setting_find_array_element(setting_ref_custom_subsystem_id(ref),
                                                       setting_ref_key(ref), ref->array_index)
               : zmk_custom_setting_find(setting_ref_custom_subsystem_id(ref),
                                         setting_ref_key(ref));
}

static const struct zmk_custom_setting *
array_for_ref(const struct zmk_custom_settings_setting_ref *ref) {
    if (!ref->has_key) {
        return NULL;
    }

    return ref->has_array_index
               ? zmk_custom_setting_find_array_element(setting_ref_custom_subsystem_id(ref),
                                                       setting_ref_key(ref), ref->array_index)
               : zmk_custom_setting_find_array(setting_ref_custom_subsystem_id(ref),
                                               setting_ref_key(ref));
}

static const char *
setting_scope_custom_subsystem_id(const struct zmk_custom_settings_setting_scope *scope) {
    return scope->has_custom_subsystem_id ? scope->custom_subsystem_id : NULL;
}

static const char *setting_scope_key(const struct zmk_custom_settings_setting_scope *scope) {
    return scope->has_key ? scope->key : NULL;
}

static const char *setting_scope_key_prefix(const struct zmk_custom_settings_setting_scope *scope) {
    return scope->has_key_prefix ? scope->key_prefix : NULL;
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

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int custom_subsystem_index_for_identifier(const char *identifier, uint32_t *index);
static const char *custom_subsystem_identifier_for_index(uint32_t index);
#endif

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

    struct zmk_custom_setting_value rpc_value;
    int ret = zmk_custom_setting_serialize_rpc_value(setting, src, &rpc_value);
    if (ret < 0) {
        return ret;
    }
    src = &rpc_value;

    if (zmk_custom_setting_is_array(setting)) {
        dest->which_value_type = zmk_custom_settings_SettingValue_array_value_tag;
        dest->value_type.array_value.index = setting->array_index;
        dest->value_type.array_value.size = zmk_custom_setting_array_size(setting);
        dest->value_type.array_value.has_value = true;
        return value_to_scalar_proto(src, &dest->value_type.array_value.value);
    }

    zmk_custom_settings_SettingScalarValue scalar =
        zmk_custom_settings_SettingScalarValue_init_zero;
    ret = value_to_scalar_proto(src, &scalar);
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

static int setting_meta_to_proto(const struct zmk_custom_setting *setting,
                                 zmk_custom_settings_SettingMeta *dest) {
    *dest = (zmk_custom_settings_SettingMeta)zmk_custom_settings_SettingMeta_init_zero;

    dest->confidentiality = proto_confidentiality(setting->confidentiality);
    dest->read_permission = proto_permission(setting->read_permission);
    dest->write_permission = proto_permission(setting->write_permission);

    for (size_t i = 0;
         i < setting->constraints_count && dest->constraints_count < ARRAY_SIZE(dest->constraints);
         i++) {
        if (setting->constraints[i].type == ZMK_CUSTOM_SETTING_CONSTRAINT_NONE) {
            continue;
        }

        int ret = constraint_to_proto(&setting->constraints[i],
                                      &dest->constraints[dest->constraints_count]);
        if (ret < 0) {
            continue;
        }
        dest->constraints_count++;
    }

    return 0;
}

static int setting_value_to_proto(const struct zmk_custom_setting *setting,
                                  zmk_custom_settings_SettingValue *dest) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read(setting, &value);
    if (ret < 0) {
        return ret;
    }

    return value_to_proto(setting, &value, dest);
}

static int setting_to_proto(const struct zmk_custom_setting *setting,
                            zmk_custom_settings_Setting *dest, bool include_value,
                            bool include_meta, uint32_t source) {
    *dest = (zmk_custom_settings_Setting)zmk_custom_settings_Setting_init_zero;

    int ret;
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    uint32_t custom_subsystem_index = 0;
    ret = custom_subsystem_index_for_identifier(setting->custom_subsystem_id,
                                                &custom_subsystem_index);
    if (ret < 0) {
        return ret;
    }
    dest->custom_subsystem_index = custom_subsystem_index;
#else
    dest->custom_subsystem_index = 0;
#endif

    copy_string(dest->key, sizeof(dest->key), zmk_custom_setting_public_key(setting));
    dest->has_unsaved_value = zmk_custom_setting_has_unsaved_value(setting);
    dest->source = source;

    if (include_meta) {
        dest->has_meta = true;
        ret = setting_meta_to_proto(setting, &dest->meta);
        if (ret < 0) {
            dest->has_meta = false;
            return ret;
        }
    }

    if (include_value &&
        setting->confidentiality != ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE) {
        dest->has_value = true;
        ret = setting_value_to_proto(setting, &dest->value);
        if (ret < 0) {
            dest->has_value = false;
            return ret;
        }
    }

    return 0;
}

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static const char *custom_subsystem_identifier_for_index(uint32_t index) {
    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    if (index >= subsystem_count) {
        return NULL;
    }

    struct zmk_rpc_custom_subsystem *custom_subsys;
    STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, index, &custom_subsys);
    return custom_subsys->identifier;
}

static int custom_subsystem_index_for_identifier(const char *identifier, uint32_t *index) {
    if (!identifier) {
        return -ENOENT;
    }

    size_t subsystem_count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &subsystem_count);

    for (size_t i = 0; i < subsystem_count; i++) {
        struct zmk_rpc_custom_subsystem *custom_subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &custom_subsys);
        if (strcmp(custom_subsys->identifier, identifier) == 0) {
            *index = i;
            return 0;
        }
    }

    return -ENOENT;
}

static int custom_subsystem_index(void) {
    uint32_t index;
    int ret = custom_subsystem_index_for_identifier(SUBSYSTEM_IDENTIFIER_STRING, &index);
    if (ret < 0) {
        return ret;
    }

    return (int)index;
}

static bool encode_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const zmk_custom_settings_Notification *notification =
        (const zmk_custom_settings_Notification *)*arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, zmk_custom_settings_Notification_fields, notification);
}
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int relayed_notification_to_public(const struct zmk_custom_settings_relay_notification *ev,
                                          zmk_custom_settings_Notification *notification) {
    pb_istream_t stream = pb_istream_from_buffer(ev->payload, ev->payload_size);
    if (!pb_decode(&stream, zmk_custom_settings_Notification_fields, notification)) {
        LOG_WRN("Failed to decode relayed custom settings notification: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }

    if (notification->which_notification_type != zmk_custom_settings_Notification_setting_tag ||
        !notification->notification_type.setting.has_setting) {
        return 0;
    }

    uint32_t custom_subsystem_index = 0;
    int ret =
        custom_subsystem_index_for_identifier(ev->custom_subsystem_id, &custom_subsystem_index);
    if (ret < 0) {
        return ret;
    }

    notification->notification_type.setting.setting.custom_subsystem_index = custom_subsystem_index;

    return 0;
}

static int raise_encoded_studio_notification(const zmk_custom_settings_Notification *notification) {
    int index = custom_subsystem_index();
    if (index < 0) {
        return index;
    }

    pb_callback_t payload = {
        .funcs.encode = encode_notification_payload,
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
                                      bool include_value, bool include_meta, uint32_t source) {
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zmk_custom_settings_Notification notification = zmk_custom_settings_Notification_init_zero;
    notification.which_notification_type = zmk_custom_settings_Notification_setting_tag;
    notification.notification_type.setting.kind = kind;
    notification.notification_type.setting.has_setting = true;
    int ret = setting_to_proto(setting, &notification.notification_type.setting.setting,
                               include_value, include_meta, source);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_settings_relay_notification relay_notification = {
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    };
    copy_string(relay_notification.custom_subsystem_id,
                sizeof(relay_notification.custom_subsystem_id), setting->custom_subsystem_id);
    pb_ostream_t stream =
        pb_ostream_from_buffer(relay_notification.payload, sizeof(relay_notification.payload));
    if (!pb_encode(&stream, zmk_custom_settings_Notification_fields, &notification)) {
        LOG_WRN("Failed to encode custom settings relay notification: %s", PB_GET_ERROR(&stream));
        return -EIO;
    }
    relay_notification.payload_size = stream.bytes_written;
    return raise_zmk_custom_settings_relay_notification(relay_notification);
#else
    zmk_custom_settings_Notification notification = zmk_custom_settings_Notification_init_zero;
    notification.which_notification_type = zmk_custom_settings_Notification_setting_tag;
    notification.notification_type.setting.kind = kind;
    notification.notification_type.setting.has_setting = true;
    int ret = setting_to_proto(setting, &notification.notification_type.setting.setting,
                               include_value, include_meta, source);
    if (ret < 0) {
        return ret;
    }

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
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
#else
    return 0;
#endif
#endif
}

static bool setting_is_active(const struct zmk_custom_setting *setting) {
    return !zmk_custom_setting_is_array(setting) ||
           setting->array_index < zmk_custom_setting_array_size(setting);
}

static bool can_include_value(const struct zmk_custom_setting *setting) {
    return setting_is_active(setting) &&
           setting->confidentiality != ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE &&
           !needs_unlock(setting->read_permission);
}

static void list_settings_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(list_settings_work, list_settings_work_handler);
static K_MUTEX_DEFINE(list_settings_lock);
static struct zmk_custom_settings_setting_scope list_settings_scope;
static size_t list_settings_next_index;
static bool list_settings_active;
static bool list_settings_include_meta;

static int schedule_list_settings_work(k_timeout_t delay) {
    return k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &list_settings_work, delay);
}

static bool setting_matches_scope(const struct zmk_custom_setting *setting,
                                  const struct zmk_custom_settings_setting_scope *scope) {
    return zmk_custom_setting_matches(setting, setting_scope_custom_subsystem_id(scope),
                                      setting_scope_key(scope), setting_scope_key_prefix(scope));
}

static void list_settings_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&list_settings_lock, K_FOREVER);
    if (!list_settings_active) {
        k_mutex_unlock(&list_settings_lock);
        return;
    }
    struct zmk_custom_settings_setting_scope scope = list_settings_scope;
    size_t next_index = list_settings_next_index;
    bool include_meta = list_settings_include_meta;
    k_mutex_unlock(&list_settings_lock);

    size_t index = 0;
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (index++ < next_index || !setting_is_active(setting) ||
            !setting_matches_scope(setting, &scope)) {
            continue;
        }

        k_mutex_lock(&list_settings_lock, K_FOREVER);
        list_settings_next_index = index;
        k_mutex_unlock(&list_settings_lock);

        LOG_INF("Custom settings list item: subsystem=%s key=%s index=%u include_value=%d "
                "include_meta=%d",
                setting->custom_subsystem_id, zmk_custom_setting_public_key(setting),
                (uint32_t)(zmk_custom_setting_is_array(setting) ? setting->array_index
                                                                : ZMK_CUSTOM_SETTING_ARRAY_NONE),
                can_include_value(setting), include_meta);
        int ret = raise_setting_notification(
            setting,
            zmk_custom_settings_SettingNotificationKind_SETTING_NOTIFICATION_KIND_LIST_ITEM,
            can_include_value(setting), include_meta, ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
        if (ret < 0) {
            LOG_WRN("Failed to raise custom settings list notification: %d", ret);
        }

        schedule_list_settings_work(LIST_SETTINGS_NOTIFICATION_DELAY);
        return;
    }

    k_mutex_lock(&list_settings_lock, K_FOREVER);
    list_settings_active = false;
    k_mutex_unlock(&list_settings_lock);
    LOG_INF("Custom settings list complete");
}

static void schedule_list_settings(const struct zmk_custom_settings_setting_scope *scope,
                                   bool include_meta) {
    k_work_cancel_delayable(&list_settings_work);

    k_mutex_lock(&list_settings_lock, K_FOREVER);
    list_settings_scope = *scope;
    list_settings_next_index = 0;
    list_settings_active = true;
    list_settings_include_meta = include_meta;
    k_mutex_unlock(&list_settings_lock);

    LOG_INF(
        "Custom settings list scheduled: subsystem=%s key=%s prefix=%s source=%u include_meta=%d",
        setting_scope_custom_subsystem_id(scope) ? setting_scope_custom_subsystem_id(scope) : "",
        setting_scope_key(scope) ? setting_scope_key(scope) : "",
        setting_scope_key_prefix(scope) ? setting_scope_key_prefix(scope) : "",
        setting_scope_source(scope), include_meta);
    schedule_list_settings_work(LIST_SETTINGS_NOTIFICATION_DELAY);
}

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int setting_ref_to_private(const zmk_custom_settings_SettingRef *src,
                                  struct zmk_custom_settings_setting_ref *dest) {
    *dest = (struct zmk_custom_settings_setting_ref){0};

    if (src->has_custom_subsystem_index) {
        const char *identifier = custom_subsystem_identifier_for_index(src->custom_subsystem_index);
        if (!identifier) {
            return -ENOENT;
        }
        dest->has_custom_subsystem_id = true;
        copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id), identifier);
    }
    if (src->has_key) {
        dest->has_key = true;
        copy_string(dest->key, sizeof(dest->key), src->key);
    }
    if (src->has_source) {
        dest->has_source = true;
        dest->source = src->source;
    }
    if (src->has_array_index) {
        dest->has_array_index = true;
        dest->array_index = src->array_index;
    }

    return 0;
}

static int setting_scope_to_private(const zmk_custom_settings_SettingScope *src,
                                    struct zmk_custom_settings_setting_scope *dest) {
    *dest = (struct zmk_custom_settings_setting_scope){0};

    if (src->has_custom_subsystem_index) {
        const char *identifier = custom_subsystem_identifier_for_index(src->custom_subsystem_index);
        if (!identifier) {
            return -ENOENT;
        }
        dest->has_custom_subsystem_id = true;
        copy_string(dest->custom_subsystem_id, sizeof(dest->custom_subsystem_id), identifier);
    }
    if (src->has_key) {
        dest->has_key = true;
        copy_string(dest->key, sizeof(dest->key), src->key);
    }
    if (src->has_key_prefix) {
        dest->has_key_prefix = true;
        copy_string(dest->key_prefix, sizeof(dest->key_prefix), src->key_prefix);
    }
    if (src->has_source) {
        dest->has_source = true;
        dest->source = src->source;
    }

    return 0;
}
#endif

static int handle_private_list_settings(const struct zmk_custom_settings_setting_scope *scope,
                                        bool require_meta, zmk_custom_settings_Response *resp) {
    uint32_t count = 0;

    if (!source_targets_local(setting_scope_source(scope))) {
        set_status(resp, 0, "No local settings matched source");
        return 0;
    }

    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (!setting_is_active(setting) || !setting_matches_scope(setting, scope)) {
            continue;
        }

        count++;
    }

    if (count > 0) {
        schedule_list_settings(scope, require_meta);
    }

    LOG_INF("Custom settings list request accepted: count=%u subsystem=%s key=%s prefix=%s "
            "source=%u require_meta=%d",
            count,
            setting_scope_custom_subsystem_id(scope) ? setting_scope_custom_subsystem_id(scope)
                                                     : "",
            setting_scope_key(scope) ? setting_scope_key(scope) : "",
            setting_scope_key_prefix(scope) ? setting_scope_key_prefix(scope) : "",
            setting_scope_source(scope), require_meta);
    set_status(resp, count, "List started");
    return 0;
}

static int handle_list_settings(const zmk_custom_settings_ListSettingsRequest *req,
                                zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_scope scope;
    int ret = setting_scope_to_private(&req->scope, &scope);
    if (ret < 0) {
        return ret;
    }

    return handle_private_list_settings(&scope, req->require_meta, resp);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_get_setting(const struct zmk_custom_settings_setting_ref *ref,
                                      zmk_custom_settings_Response *resp, bool include_meta) {
    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        return -ENOENT;
    }

    const struct zmk_custom_setting *setting = setting_for_ref(ref);
    if (!setting) {
        return -ENOENT;
    }

    if (needs_unlock(setting->read_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    zmk_custom_settings_GetSettingResponse get = zmk_custom_settings_GetSettingResponse_init_zero;
    get.has_setting = true;
    int ret = setting_to_proto(setting, &get.setting, true, include_meta,
                               ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }

    resp->which_response_type = zmk_custom_settings_Response_get_setting_tag;
    resp->response_type.get_setting = get;
    return 0;
}

static int handle_get_setting(const zmk_custom_settings_GetSettingRequest *req,
                              zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    return handle_private_get_setting(&private_ref, resp, req->require_meta);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_write_setting(const struct zmk_custom_settings_setting_ref *ref,
                                        const struct zmk_custom_setting_value *value,
                                        bool value_is_array, uint32_t array_index,
                                        uint32_t array_size,
                                        zmk_custom_settings_SettingWriteMode write_mode,
                                        zmk_custom_settings_Response *resp,
                                        bool value_uses_rpc_format) {
    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    struct zmk_custom_settings_setting_ref resolved_ref = *ref;
    if (value_is_array || ref->has_array_index) {
        uint32_t resolved_index = value_is_array ? array_index : ref->array_index;
        if (value_is_array && ref->has_array_index && ref->array_index != array_index) {
            return -EINVAL;
        }

        resolved_ref.has_array_index = true;
        resolved_ref.array_index = resolved_index;
    }

    const struct zmk_custom_setting *setting = setting_for_ref(&resolved_ref);
    if (!setting) {
        return -ENOENT;
    }
    if (value_is_array) {
        if (array_size == 0 || array_size > zmk_custom_setting_array_max_size(setting) ||
            setting->array_index >= array_size) {
            return -EINVAL;
        }
    }

    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        write_mode == zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    struct zmk_custom_setting_value internal_value;
    int ret = 0;
    if (value_uses_rpc_format) {
        ret = zmk_custom_setting_deserialize_rpc_value(setting, value, &internal_value);
        if (ret < 0) {
            return ret;
        }
        value = &internal_value;
    }

    ret = value_is_array ? zmk_custom_setting_write_array_element(setting, value, array_size, mode)
                         : zmk_custom_setting_write(setting, value, mode);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Setting written");
    return 0;
}

static int handle_write_setting(const zmk_custom_settings_WriteSettingRequest *req,
                                zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value value;
    bool value_is_array = false;
    uint32_t array_index = 0;
    uint32_t array_size = 0;
    ret = proto_to_value(&req->value, &value, &value_is_array, &array_index, &array_size);
    if (ret < 0) {
        return ret;
    }

    return handle_private_write_setting(&private_ref, &value, value_is_array, array_index,
                                        array_size, req->mode, resp, true);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_push_back_array(const struct zmk_custom_settings_setting_ref *ref,
                                          const struct zmk_custom_setting_value *value,
                                          zmk_custom_settings_SettingWriteMode write_mode,
                                          zmk_custom_settings_Response *resp,
                                          bool value_uses_rpc_format) {
    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    const struct zmk_custom_setting *setting = array_for_ref(ref);
    if (!setting) {
        return -ENOENT;
    }

    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        write_mode == zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    struct zmk_custom_setting_value internal_value;
    int ret = 0;
    if (value_uses_rpc_format) {
        ret = zmk_custom_setting_deserialize_rpc_value(setting, value, &internal_value);
        if (ret < 0) {
            return ret;
        }
        value = &internal_value;
    }

    ret = zmk_custom_setting_array_push_back(setting, value, mode);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Array value pushed");
    return 0;
}

static int handle_push_back_array(const zmk_custom_settings_PushBackArrayRequest *req,
                                  zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value value;
    ret = scalar_proto_to_value(&req->value, &value);
    if (ret < 0) {
        return ret;
    }

    return handle_private_push_back_array(&private_ref, &value, req->mode, resp, true);
#else
    return -ENOTSUP;
#endif
}

static int handle_private_pop_back_array(const struct zmk_custom_settings_setting_ref *ref,
                                         zmk_custom_settings_SettingWriteMode write_mode,
                                         zmk_custom_settings_Response *resp) {
    if (!ref->has_key) {
        return -EINVAL;
    }

    if (!source_targets_local(setting_ref_source(ref))) {
        set_status(resp, 0, "No local setting matched source");
        return 0;
    }

    const struct zmk_custom_setting *setting = array_for_ref(ref);
    if (!setting) {
        return -ENOENT;
    }

    if (needs_unlock(setting->write_permission)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    enum zmk_custom_setting_write_mode mode =
        write_mode == zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_PERSIST
            ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
            : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY;

    int ret = zmk_custom_setting_array_pop_back(setting, NULL, mode);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, 1, "Array value popped");
    return 0;
}

static int handle_pop_back_array(const zmk_custom_settings_PopBackArrayRequest *req,
                                 zmk_custom_settings_Response *resp) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_ref private_ref;
    int ret = setting_ref_to_private(&req->setting, &private_ref);
    if (ret < 0) {
        return ret;
    }

    return handle_private_pop_back_array(&private_ref, req->mode, resp);
#else
    return -ENOTSUP;
#endif
}

static bool scope_has_permission(const struct zmk_custom_settings_setting_scope *scope,
                                 enum zmk_custom_setting_permission permission,
                                 bool read_permission) {
    ZMK_CUSTOM_SETTING_FOREACH(setting) {
        if (setting_matches_scope(setting, scope) &&
            (read_permission ? setting->read_permission : setting->write_permission) ==
                permission) {
            return true;
        }
    }

    return false;
}

static bool scope_write_unlock_required(const struct zmk_custom_settings_setting_scope *scope) {
    return !studio_is_unlocked() &&
           scope_has_permission(scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, false);
}

static int handle_private_scope_mutation(const struct zmk_custom_settings_setting_scope *scope,
                                         zmk_custom_settings_Response *resp, const char *message,
                                         int (*callback)(const char *, const char *, const char *,
                                                         uint32_t *)) {
    if (!source_targets_local(setting_scope_source(scope))) {
        set_status(resp, 0, "No local settings matched source");
        return 0;
    }

    if (scope_write_unlock_required(scope)) {
        set_error(resp, "Unlock required");
        return 0;
    }

    uint32_t count = 0;
    int ret = callback(setting_scope_custom_subsystem_id(scope), setting_scope_key(scope),
                       setting_scope_key_prefix(scope), &count);
    if (ret < 0) {
        return ret;
    }

    set_status(resp, count, message);
    return 0;
}

static int handle_scope_mutation(const zmk_custom_settings_SettingScope *scope,
                                 zmk_custom_settings_Response *resp, const char *message,
                                 int (*callback)(const char *, const char *, const char *,
                                                 uint32_t *)) {
#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
    struct zmk_custom_settings_setting_scope private_scope;
    int ret = setting_scope_to_private(scope, &private_scope);
    if (ret < 0) {
        return ret;
    }

    return handle_private_scope_mutation(&private_scope, resp, message, callback);
#else
    return -ENOTSUP;
#endif
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static bool should_relay_to_peripherals(const zmk_custom_settings_Request *req) {
    switch (req->which_request_type) {
    case zmk_custom_settings_Request_list_settings_tag:
        return scope_source(&req->request_type.list_settings.scope) ==
               ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_save_settings_tag:
        return scope_source(&req->request_type.save_settings.scope) ==
               ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_discard_settings_tag:
        return scope_source(&req->request_type.discard_settings.scope) ==
               ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_reset_settings_tag:
        return scope_source(&req->request_type.reset_settings.scope) ==
               ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_write_setting_tag:
        return ref_source(&req->request_type.write_setting.setting) ==
               ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_push_back_array_tag:
        return ref_source(&req->request_type.push_back_array.setting) ==
               ZMK_CUSTOM_SETTING_SOURCE_ALL;
    case zmk_custom_settings_Request_pop_back_array_tag:
        return ref_source(&req->request_type.pop_back_array.setting) ==
               ZMK_CUSTOM_SETTING_SOURCE_ALL;
    default:
        return false;
    }
}

static bool relay_request_unlock_required(const zmk_custom_settings_Request *req) {
    if (studio_is_unlocked()) {
        return false;
    }

    switch (req->which_request_type) {
    case zmk_custom_settings_Request_list_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        if (setting_scope_to_private(&req->request_type.list_settings.scope, &scope) < 0) {
            return false;
        }

        return scope_has_permission(&scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, true);
    }
    case zmk_custom_settings_Request_write_setting_tag: {
        struct zmk_custom_settings_setting_ref ref;
        if (setting_ref_to_private(&req->request_type.write_setting.setting, &ref) < 0) {
            return false;
        }

        struct zmk_custom_setting_value value;
        bool value_is_array = false;
        uint32_t array_index = 0;
        uint32_t array_size = 0;
        if (proto_to_value(&req->request_type.write_setting.value, &value, &value_is_array,
                           &array_index, &array_size) < 0) {
            return false;
        }
        if (value_is_array) {
            ref.has_array_index = true;
            ref.array_index = array_index;
        }

        const struct zmk_custom_setting *setting = setting_for_ref(&ref);
        return setting && setting->write_permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE;
    }
    case zmk_custom_settings_Request_push_back_array_tag: {
        struct zmk_custom_settings_setting_ref ref;
        if (setting_ref_to_private(&req->request_type.push_back_array.setting, &ref) < 0) {
            return false;
        }

        const struct zmk_custom_setting *setting = array_for_ref(&ref);
        return setting && setting->write_permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE;
    }
    case zmk_custom_settings_Request_pop_back_array_tag: {
        struct zmk_custom_settings_setting_ref ref;
        if (setting_ref_to_private(&req->request_type.pop_back_array.setting, &ref) < 0) {
            return false;
        }

        const struct zmk_custom_setting *setting = array_for_ref(&ref);
        return setting && setting->write_permission == ZMK_CUSTOM_SETTING_PERMISSION_SECURE;
    }
    case zmk_custom_settings_Request_save_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        if (setting_scope_to_private(&req->request_type.save_settings.scope, &scope) < 0) {
            return false;
        }

        return scope_has_permission(&scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, false);
    }
    case zmk_custom_settings_Request_discard_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        if (setting_scope_to_private(&req->request_type.discard_settings.scope, &scope) < 0) {
            return false;
        }

        return scope_has_permission(&scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, false);
    }
    case zmk_custom_settings_Request_reset_settings_tag: {
        struct zmk_custom_settings_setting_scope scope;
        if (setting_scope_to_private(&req->request_type.reset_settings.scope, &scope) < 0) {
            return false;
        }

        return scope_has_permission(&scope, ZMK_CUSTOM_SETTING_PERMISSION_SECURE, false);
    }
    default:
        return false;
    }
}
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int value_to_relay_value(const struct zmk_custom_setting_value *src,
                                struct zmk_custom_settings_relay_value *dest) {
    *dest = (struct zmk_custom_settings_relay_value){0};
    dest->type = src->type;
    dest->size = src->size;

    switch (src->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        if (src->size > sizeof(dest->bytes_value)) {
            return -EMSGSIZE;
        }
        memcpy(dest->bytes_value, src->bytes_value, src->size);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        dest->int32_value = src->int32_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        dest->bool_value = src->bool_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        copy_string(dest->string_value, sizeof(dest->string_value), src->string_value);
        return 0;
    default:
        return -EINVAL;
    }
}

static int request_to_relay_request(const zmk_custom_settings_Request *src,
                                    struct zmk_custom_settings_relay_request *dest) {
    uint8_t source = dest->source;
    *dest = (struct zmk_custom_settings_relay_request){
        .source = source,
    };

    switch (src->which_request_type) {
    case zmk_custom_settings_Request_list_settings_tag:
        dest->type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_LIST;
        dest->request.list_settings.require_meta = src->request_type.list_settings.require_meta;
        return setting_scope_to_private(&src->request_type.list_settings.scope,
                                        &dest->request.list_settings.scope);
    case zmk_custom_settings_Request_write_setting_tag: {
        dest->type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_WRITE;
        dest->request.write_setting.mode = src->request_type.write_setting.mode;
        int ret = setting_ref_to_private(&src->request_type.write_setting.setting,
                                         &dest->request.write_setting.setting);
        if (ret < 0) {
            return ret;
        }

        struct zmk_custom_setting_value value;
        bool value_is_array = false;
        uint32_t array_index = 0;
        uint32_t array_size = 0;
        ret = proto_to_value(&src->request_type.write_setting.value, &value, &value_is_array,
                             &array_index, &array_size);
        if (ret < 0) {
            return ret;
        }
        dest->request.write_setting.value_is_array = value_is_array;
        dest->request.write_setting.array_index = array_index;
        dest->request.write_setting.array_size = array_size;
        return value_to_relay_value(&value, &dest->request.write_setting.value);
    }
    case zmk_custom_settings_Request_push_back_array_tag: {
        dest->type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_PUSH_BACK;
        dest->request.push_back_array.mode = src->request_type.push_back_array.mode;
        int ret = setting_ref_to_private(&src->request_type.push_back_array.setting,
                                         &dest->request.push_back_array.setting);
        if (ret < 0) {
            return ret;
        }

        struct zmk_custom_setting_value value;
        ret = scalar_proto_to_value(&src->request_type.push_back_array.value, &value);
        if (ret < 0) {
            return ret;
        }

        return value_to_relay_value(&value, &dest->request.push_back_array.value);
    }
    case zmk_custom_settings_Request_pop_back_array_tag:
        dest->type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_POP_BACK;
        dest->request.pop_back_array.mode = src->request_type.pop_back_array.mode;
        return setting_ref_to_private(&src->request_type.pop_back_array.setting,
                                      &dest->request.pop_back_array.setting);
    case zmk_custom_settings_Request_save_settings_tag:
        dest->type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_SAVE;
        return setting_scope_to_private(&src->request_type.save_settings.scope,
                                        &dest->request.scope_mutation.scope);
    case zmk_custom_settings_Request_discard_settings_tag:
        dest->type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_DISCARD;
        return setting_scope_to_private(&src->request_type.discard_settings.scope,
                                        &dest->request.scope_mutation.scope);
    case zmk_custom_settings_Request_reset_settings_tag:
        dest->type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_RESET;
        return setting_scope_to_private(&src->request_type.reset_settings.scope,
                                        &dest->request.scope_mutation.scope);
    default:
        return -ENOTSUP;
    }
}

static int relay_request_to_peripherals(const zmk_custom_settings_Request *req) {
    struct zmk_custom_settings_relay_request relay_request = {
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    };

    int ret = request_to_relay_request(req, &relay_request);
    if (ret < 0) {
        return ret;
    }

    return raise_zmk_custom_settings_relay_request(relay_request);
}

static void list_settings_relay_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(list_settings_relay_work, list_settings_relay_work_handler);
static K_MUTEX_DEFINE(list_settings_relay_lock);
static struct zmk_custom_settings_relay_request list_settings_relay_request;
static bool list_settings_relay_active;

static void list_settings_relay_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&list_settings_relay_lock, K_FOREVER);
    if (!list_settings_relay_active) {
        k_mutex_unlock(&list_settings_relay_lock);
        return;
    }
    struct zmk_custom_settings_relay_request relay_request = list_settings_relay_request;
    list_settings_relay_active = false;
    k_mutex_unlock(&list_settings_relay_lock);

    LOG_INF("Custom settings relaying list request to peripherals");
    int ret = raise_zmk_custom_settings_relay_request(relay_request);
    if (ret < 0) {
        LOG_WRN("Failed to relay custom settings list request: %d", ret);
    }
}

static int schedule_list_settings_relay_to_peripherals(const zmk_custom_settings_Request *req) {
    struct zmk_custom_settings_relay_request relay_request = {
        .source = ZMK_RELAY_EVENT_SOURCE_SELF,
    };

    int ret = request_to_relay_request(req, &relay_request);
    if (ret < 0) {
        return ret;
    }

    k_work_cancel_delayable(&list_settings_relay_work);
    k_mutex_lock(&list_settings_relay_lock, K_FOREVER);
    list_settings_relay_request = relay_request;
    list_settings_relay_active = true;
    k_mutex_unlock(&list_settings_relay_lock);

    LOG_INF("Custom settings scheduled list relay to peripherals");
    k_work_schedule(&list_settings_relay_work, LIST_SETTINGS_RELAY_DELAY);
    return 0;
}
#endif

static int process_request(const zmk_custom_settings_Request *req,
                           zmk_custom_settings_Response *resp) {
    switch (req->which_request_type) {
    case zmk_custom_settings_Request_list_settings_tag:
        return handle_list_settings(&req->request_type.list_settings, resp);
    case zmk_custom_settings_Request_get_setting_tag:
        return handle_get_setting(&req->request_type.get_setting, resp);
    case zmk_custom_settings_Request_write_setting_tag:
        return handle_write_setting(&req->request_type.write_setting, resp);
    case zmk_custom_settings_Request_push_back_array_tag:
        return handle_push_back_array(&req->request_type.push_back_array, resp);
    case zmk_custom_settings_Request_pop_back_array_tag:
        return handle_pop_back_array(&req->request_type.pop_back_array, resp);
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

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
static int relay_value_to_value(const struct zmk_custom_settings_relay_value *src,
                                struct zmk_custom_setting_value *dest) {
    *dest = (struct zmk_custom_setting_value){0};
    dest->type = src->type;
    dest->size = src->size;

    switch (src->type) {
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES:
        if (src->size > sizeof(dest->bytes_value)) {
            return -EMSGSIZE;
        }
        memcpy(dest->bytes_value, src->bytes_value, src->size);
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32:
        dest->int32_value = src->int32_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL:
        dest->bool_value = src->bool_value;
        return 0;
    case ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING:
        copy_string(dest->string_value, sizeof(dest->string_value), src->string_value);
        dest->size = bounded_strlen(dest->string_value, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
        return 0;
    default:
        return -EINVAL;
    }
}

static int process_relay_request(const struct zmk_custom_settings_relay_request *req,
                                 zmk_custom_settings_Response *resp) {
    switch (req->type) {
    case ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_LIST:
        return handle_private_list_settings(&req->request.list_settings.scope,
                                            req->request.list_settings.require_meta, resp);
    case ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_WRITE: {
        struct zmk_custom_setting_value value;
        int ret = relay_value_to_value(&req->request.write_setting.value, &value);
        if (ret < 0) {
            return ret;
        }
        return handle_private_write_setting(
            &req->request.write_setting.setting, &value, req->request.write_setting.value_is_array,
            req->request.write_setting.array_index, req->request.write_setting.array_size,
            (zmk_custom_settings_SettingWriteMode)req->request.write_setting.mode, resp, true);
    }
    case ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_PUSH_BACK: {
        struct zmk_custom_setting_value value;
        int ret = relay_value_to_value(&req->request.push_back_array.value, &value);
        if (ret < 0) {
            return ret;
        }
        return handle_private_push_back_array(
            &req->request.push_back_array.setting, &value,
            (zmk_custom_settings_SettingWriteMode)req->request.push_back_array.mode, resp, true);
    }
    case ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_POP_BACK:
        return handle_private_pop_back_array(
            &req->request.pop_back_array.setting,
            (zmk_custom_settings_SettingWriteMode)req->request.pop_back_array.mode, resp);
    case ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_SAVE:
        return handle_private_scope_mutation(&req->request.scope_mutation.scope, resp,
                                             "Settings saved", zmk_custom_settings_save_scope);
    case ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_DISCARD:
        return handle_private_scope_mutation(&req->request.scope_mutation.scope, resp,
                                             "Settings discarded",
                                             zmk_custom_settings_discard_scope);
    case ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_RESET:
        return handle_private_scope_mutation(&req->request.scope_mutation.scope, resp,
                                             "Settings reset", zmk_custom_settings_reset_scope);
    default:
        return -ENOTSUP;
    }
}
#endif

#if ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
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
    LOG_INF("Custom settings RPC request: type=%u payload_size=%u", req.which_request_type,
            (uint32_t)raw_request->payload.size);

    int ret = process_request(&req, resp);
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (ret == 0 && should_relay_to_peripherals(&req)) {
        if (!relay_request_unlock_required(&req)) {
            if (req.which_request_type == zmk_custom_settings_Request_list_settings_tag) {
                ret = schedule_list_settings_relay_to_peripherals(&req);
            } else {
                ret = relay_request_to_peripherals(&req);
            }
        } else {
            LOG_INF("Custom settings skipped peripheral relay because unlock is required");
        }
    }
#endif
    LOG_INF("Custom settings RPC request processed: ret=%d response_type=%u", ret,
            resp->which_response_type);

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
#endif

static int setting_changed_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *ev = as_zmk_custom_setting_changed(eh);
    if (!ev || !ev->setting) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    raise_setting_notification(ev->setting, proto_notification_kind(ev->kind),
                               can_include_value(ev->setting), false, ev->source);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(custom_settings_studio, setting_changed_listener);
ZMK_SUBSCRIPTION(custom_settings_studio, zmk_custom_setting_changed);

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int custom_settings_rpc_bytes_converter_test_init(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "bytes_value");
    if (!setting) {
        LOG_ERR("RPC bytes converter test setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    zmk_custom_settings_Setting proto_setting = zmk_custom_settings_Setting_init_zero;
    ret = setting_to_proto(setting, &proto_setting, true, false, ZMK_CUSTOM_SETTING_SOURCE_LOCAL);
    if (ret < 0) {
        return ret;
    }
    if (!proto_setting.has_value ||
        proto_setting.value.which_value_type != zmk_custom_settings_SettingValue_bytes_value_tag ||
        proto_setting.value.value_type.bytes_value.size != 3 ||
        proto_setting.value.value_type.bytes_value.bytes[0] != 3 ||
        proto_setting.value.value_type.bytes_value.bytes[1] != 2 ||
        proto_setting.value.value_type.bytes_value.bytes[2] != 1) {
        LOG_ERR("RPC bytes converter test serialization failed");
        return -EINVAL;
    }

    struct zmk_custom_settings_setting_ref ref = {
        .has_custom_subsystem_id = true,
        .has_key = true,
        .has_source = true,
        .source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL,
    };
    copy_string(ref.custom_subsystem_id, sizeof(ref.custom_subsystem_id), "test");
    copy_string(ref.key, sizeof(ref.key), "bytes_value");

    struct zmk_custom_setting_value rpc_value = ZMK_CUSTOM_SETTING_VALUE_BYTES(9, 8, 7);
    zmk_custom_settings_Response resp = zmk_custom_settings_Response_init_zero;
    ret = handle_private_write_setting(
        &ref, &rpc_value, false, 0, 0,
        zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY, &resp, true);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value internal_value;
    ret = zmk_custom_setting_read(setting, &internal_value);
    if (ret < 0) {
        return ret;
    }
    if (internal_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES || internal_value.size != 3 ||
        internal_value.bytes_value[0] != 7 || internal_value.bytes_value[1] != 8 ||
        internal_value.bytes_value[2] != 9) {
        LOG_ERR("RPC bytes converter test deserialization failed");
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_rpc_bytes_handler rpc=030201 internal=070809");
    return 0;
}

SYS_INIT(custom_settings_rpc_bytes_converter_test_init, APPLICATION, 99);
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY)
static int relay_request_listener(const zmk_event_t *eh) {
    const struct zmk_custom_settings_relay_request *ev = as_zmk_custom_settings_relay_request(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_custom_settings_Response resp = zmk_custom_settings_Response_init_zero;
    int ret = process_relay_request(ev, &resp);
    if (ret < 0) {
        LOG_WRN("Relayed custom settings request failed: %d", ret);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
static int relay_notification_listener(const zmk_event_t *eh) {
    const struct zmk_custom_settings_relay_notification *ev =
        as_zmk_custom_settings_relay_notification(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->source == ZMK_RELAY_EVENT_SOURCE_SELF) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_custom_settings_Notification notification = zmk_custom_settings_Notification_init_zero;
    int ret = relayed_notification_to_public(ev, &notification);
    if (ret < 0) {
        LOG_WRN("Failed to convert relayed custom settings notification: %d", ret);
        return ZMK_EV_EVENT_BUBBLE;
    }

    raise_encoded_studio_notification(&notification);
    return ZMK_EV_EVENT_BUBBLE;
}
#endif

ZMK_LISTENER(custom_settings_relay_request, relay_request_listener);
ZMK_SUBSCRIPTION(custom_settings_relay_request, zmk_custom_settings_relay_request);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && ZMK_CUSTOM_SETTINGS_LOCAL_STUDIO_RPC
ZMK_LISTENER(custom_settings_relay_notification, relay_notification_listener);
ZMK_SUBSCRIPTION(custom_settings_relay_notification, zmk_custom_settings_relay_notification);
#endif
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_TEST) &&                                                 \
    IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY) &&                                      \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int custom_settings_split_peripheral_relay_test_init(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "int_value");
    if (!setting) {
        LOG_ERR("Split peripheral relay test setting not registered");
        return -ENOENT;
    }
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array("test", "array_value");
    const struct zmk_custom_setting *array_tail =
        zmk_custom_setting_find_array_element("test", "array_value", 2);
    if (!array_setting || !array_tail) {
        LOG_ERR("Split peripheral relay array setting not registered");
        return -ENOENT;
    }

    struct zmk_custom_settings_relay_request request = {
        .source = 1,
        .type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_WRITE,
    };
    request.request.write_setting.setting.has_custom_subsystem_id = true;
    copy_string(request.request.write_setting.setting.custom_subsystem_id,
                sizeof(request.request.write_setting.setting.custom_subsystem_id), "test");
    request.request.write_setting.setting.has_key = true;
    copy_string(request.request.write_setting.setting.key,
                sizeof(request.request.write_setting.setting.key), "int_value");
    request.request.write_setting.setting.has_source = true;
    request.request.write_setting.setting.source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
    request.request.write_setting.value.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
    request.request.write_setting.value.int32_value = 66;
    request.request.write_setting.mode =
        zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY;

    zmk_custom_settings_Response resp = zmk_custom_settings_Response_init_zero;
    int ret = process_relay_request(&request, &resp);
    if (ret < 0) {
        LOG_ERR("Split peripheral relay request failed: %d", ret);
        return ret;
    }

    struct zmk_custom_setting_value value;
    ret = zmk_custom_setting_read(setting, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != 66) {
        LOG_ERR("Split peripheral relay test write failed");
        return -EINVAL;
    }

    printk("PASS: split_peripheral_relay\n");

    ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_settings_relay_request pop_request = {
        .source = 1,
        .type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_POP_BACK,
    };
    pop_request.request.pop_back_array.setting.has_custom_subsystem_id = true;
    copy_string(pop_request.request.pop_back_array.setting.custom_subsystem_id,
                sizeof(pop_request.request.pop_back_array.setting.custom_subsystem_id), "test");
    pop_request.request.pop_back_array.setting.has_key = true;
    copy_string(pop_request.request.pop_back_array.setting.key,
                sizeof(pop_request.request.pop_back_array.setting.key), "array_value");
    pop_request.request.pop_back_array.setting.has_source = true;
    pop_request.request.pop_back_array.setting.source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
    pop_request.request.pop_back_array.mode =
        zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY;

    resp = (zmk_custom_settings_Response)zmk_custom_settings_Response_init_zero;
    ret = process_relay_request(&pop_request, &resp);
    if (ret < 0) {
        LOG_ERR("Split peripheral relay pop_back failed: %d", ret);
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 2) {
        LOG_ERR("Split peripheral relay pop_back did not shrink array");
        return -EINVAL;
    }
    ret = zmk_custom_setting_read(array_tail, &value);
    if (ret != -ENOENT) {
        LOG_ERR("Expected split peripheral relay popped element read to fail, got %d", ret);
        return -EINVAL;
    }
    printk("PASS: split_peripheral_relay_pop_back size=2\n");

    struct zmk_custom_settings_relay_request push_request = {
        .source = 1,
        .type = ZMK_CUSTOM_SETTINGS_RELAY_REQUEST_PUSH_BACK,
    };
    push_request.request.push_back_array.setting.has_custom_subsystem_id = true;
    copy_string(push_request.request.push_back_array.setting.custom_subsystem_id,
                sizeof(push_request.request.push_back_array.setting.custom_subsystem_id), "test");
    push_request.request.push_back_array.setting.has_key = true;
    copy_string(push_request.request.push_back_array.setting.key,
                sizeof(push_request.request.push_back_array.setting.key), "array_value");
    push_request.request.push_back_array.setting.has_source = true;
    push_request.request.push_back_array.setting.source = ZMK_CUSTOM_SETTING_SOURCE_LOCAL;
    push_request.request.push_back_array.value.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
    push_request.request.push_back_array.value.int32_value = 44;
    push_request.request.push_back_array.mode =
        zmk_custom_settings_SettingWriteMode_SETTING_WRITE_MODE_MEMORY;

    resp = (zmk_custom_settings_Response)zmk_custom_settings_Response_init_zero;
    ret = process_relay_request(&push_request, &resp);
    if (ret < 0) {
        LOG_ERR("Split peripheral relay push_back failed: %d", ret);
        return ret;
    }
    ret = zmk_custom_setting_read(array_tail, &value);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 3 ||
        value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != 44) {
        LOG_ERR("Split peripheral relay push_back did not append array value");
        return -EINVAL;
    }
    printk("PASS: split_peripheral_relay_push_back array[2]=44 size=3\n");
    return 0;
}

SYS_INIT(custom_settings_split_peripheral_relay_test_init, APPLICATION, 99);
#endif
