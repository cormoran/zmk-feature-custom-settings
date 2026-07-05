/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/behavior.h>
#include <cormoran/zmk/custom_settings.h>
#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Generous enough to also hold every "behavior/local_id/N" entry ZMK's own
 * behavior subsystem persists once test_behavior_value_type() loads the
 * "behavior" settings subtree (one entry per compiled-in behavior, ~30 for
 * the standard behaviors.dtsi set), on top of this file's own settings. */
#define TEST_SETTINGS_STORAGE_CAPACITY 64

struct test_settings_record {
    bool present;
    char name[SETTINGS_MAX_NAME_LEN];
    uint8_t data[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE];
    size_t len;
};

static struct test_settings_record test_settings_storage[TEST_SETTINGS_STORAGE_CAPACITY];

static struct test_settings_record *test_settings_find_record(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        if (test_settings_storage[i].present &&
            strncmp(test_settings_storage[i].name, name, sizeof(test_settings_storage[i].name)) ==
                0) {
            return &test_settings_storage[i];
        }
    }

    return NULL;
}

static bool test_settings_has_record(const char *name) {
    return test_settings_find_record(name) != NULL;
}

static ssize_t test_settings_read_cb(void *cb_arg, void *data, size_t len) {
    const struct test_settings_record *record = cb_arg;
    size_t read_len = MIN(record->len, len);

    memcpy(data, record->data, read_len);
    return read_len;
}

static int test_settings_load(struct settings_store *cs, const struct settings_load_arg *arg) {
    ARG_UNUSED(cs);

    int first_error = 0;
    for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
        struct test_settings_record *record = &test_settings_storage[i];
        if (!record->present) {
            continue;
        }

        int ret = settings_call_set_handler(record->name, record->len, test_settings_read_cb,
                                            record, arg);
        if (ret < 0 && first_error == 0) {
            first_error = ret;
        }
    }

    return first_error;
}

static int test_settings_save(struct settings_store *cs, const char *name, const char *value,
                              size_t val_len) {
    ARG_UNUSED(cs);

    struct test_settings_record *record = test_settings_find_record(name);
    if (value == NULL) {
        if (record) {
            record->present = false;
        }
        return 0;
    }

    if (val_len > CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE) {
        return -EMSGSIZE;
    }

    if (strlen(name) >= SETTINGS_MAX_NAME_LEN) {
        return -ENAMETOOLONG;
    }

    if (!record) {
        for (size_t i = 0; i < ARRAY_SIZE(test_settings_storage); i++) {
            if (!test_settings_storage[i].present) {
                record = &test_settings_storage[i];
                break;
            }
        }
    }

    if (!record) {
        return -ENOMEM;
    }

    record->present = true;
    strcpy(record->name, name);
    memcpy(record->data, value, val_len);
    record->len = val_len;
    return 0;
}

static const struct settings_store_itf test_settings_itf = {
    .csi_load = test_settings_load,
    .csi_save = test_settings_save,
};

static struct settings_store test_settings_store = {
    .cs_itf = &test_settings_itf,
};

static int test_settings_backend_init(void) {
    int ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }

    settings_src_register(&test_settings_store);
    settings_dst_register(&test_settings_store);
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
static struct zmk_rpc_custom_subsystem_meta test_custom_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool test_custom_settings_handle_request(const zmk_custom_CallRequest *req,
                                                pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);

    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(test, &test_custom_settings_meta, test_custom_settings_handle_request);
#endif

int test_bytes_rpc_reverse(const struct zmk_custom_setting *setting, const uint8_t *src,
                           size_t src_size, uint8_t *dest, size_t *dest_size,
                           size_t dest_capacity) {
    ARG_UNUSED(setting);

    if (src_size > dest_capacity) {
        return -EMSGSIZE;
    }

    for (size_t i = 0; i < src_size; i++) {
        dest[i] = src[src_size - i - 1];
    }
    *dest_size = src_size;
    return 0;
}

ZMK_CUSTOM_SETTING_DEFINE(test_int_setting, "test", "int_value",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(10),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS(
    test_bytes_setting, "test", "bytes_value", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    ZMK_CUSTOM_SETTING_VALUE_BYTES(1, 2, 3), ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    test_bytes_rpc_reverse, test_bytes_rpc_reverse, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(test_array_setting_0, "test", "array_value", 0, 3,
                                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                        ZMK_CUSTOM_SETTING_VALUE_INT32(1),
                                        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(test_array_setting_1, "test", "array_value", 1, 3,
                                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                        ZMK_CUSTOM_SETTING_VALUE_INT32(2),
                                        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(test_array_setting_2, "test", "array_value", 2, 3,
                                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                        ZMK_CUSTOM_SETTING_VALUE_INT32(3),
                                        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

ZMK_CUSTOM_SETTING_DEFINE(test_hid_usage_setting, "test", "hid_usage",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(4),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_HID_USAGE(HID_USAGE_KEY, 4, 29));

ZMK_CUSTOM_SETTING_DEFINE(test_layer_id_setting, "test", "layer_id",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_LAYER_ID);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
ZMK_CUSTOM_SETTING_DEFINE(test_behavior_id_setting, "test", "behavior_id",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_BEHAVIOR_ID);

ZMK_CUSTOM_SETTING_DEFINE(test_behavior_setting, "test", "behavior_value",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
                          ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(0, 0, 0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#endif

struct test_profile {
    int32_t speed;
    bool invert;
    char label[8];
};

ZMK_CUSTOM_SETTING_RECORD_RANGE_INT32_DEFINE(test_profile_speed_range, 0, 100);

ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE(
    test_profile_schema, struct test_profile,
    ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32(struct test_profile, speed, 1, &test_profile_speed_range),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_BOOL(struct test_profile, invert, 2),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_STRING(struct test_profile, label, 3));

/* Default value is just the TLV version byte (no fields), decodes to "no
 * change" against caller-provided defaults. */
ZMK_CUSTOM_SETTING_DEFINE(test_record_setting, "test", "record_value",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

static int expect_int_value(const struct zmk_custom_setting *setting, int32_t expected) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read(setting, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != expected) {
        LOG_ERR("Unexpected custom setting value: expected %d, got %d", expected,
                value.int32_value);
        return -EINVAL;
    }

    return 0;
}

static int expect_int_value_by_key(const char *custom_subsystem_id, const char *key,
                                   int32_t expected) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_by_key(custom_subsystem_id, key, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != expected) {
        LOG_ERR("Unexpected custom setting value for %s/%s: expected %d, got %d",
                custom_subsystem_id, key, expected, value.int32_value);
        return -EINVAL;
    }

    return 0;
}

static int expect_array_int_value_by_key(const char *custom_subsystem_id, const char *key,
                                         uint32_t index, int32_t expected) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(custom_subsystem_id, key, index, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || value.int32_value != expected) {
        LOG_ERR("Unexpected custom setting value for %s/%s/%u: expected %d, got %d",
                custom_subsystem_id, key, index, expected, value.int32_value);
        return -EINVAL;
    }

    return 0;
}

static int expect_bytes_value(const struct zmk_custom_setting_value *value, const uint8_t *expected,
                              size_t expected_size) {
    if (value->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES || value->size != expected_size ||
        memcmp(value->bytes_value, expected, expected_size) != 0) {
        LOG_ERR("Unexpected custom bytes setting value");
        return -EINVAL;
    }

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
static int expect_behavior_value(const struct zmk_custom_setting *setting,
                                 struct zmk_custom_setting_behavior_value expected) {
    struct zmk_custom_setting_behavior_value value;
    int ret = zmk_custom_setting_get_behavior(setting, &value);
    if (ret < 0) {
        return ret;
    }

    if (value.behavior_id != expected.behavior_id || value.param1 != expected.param1 ||
        value.param2 != expected.param2) {
        LOG_ERR("Unexpected custom behavior setting value: expected id=%u p1=%u p2=%u, got id=%u "
                "p1=%u p2=%u",
                expected.behavior_id, expected.param1, expected.param2, value.behavior_id,
                value.param1, value.param2);
        return -EINVAL;
    }

    return 0;
}
#endif

static int test_constraint_validation(void) {
    const struct zmk_custom_setting *hid_usage = zmk_custom_setting_find("test", "hid_usage");
    if (!hid_usage) {
        LOG_ERR("Test HID usage setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_write(hid_usage, &ZMK_CUSTOM_SETTING_VALUE_INT32(29),
                                       ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(hid_usage, &ZMK_CUSTOM_SETTING_VALUE_INT32(30),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected HID usage validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_validate_hid_usage");

    const struct zmk_custom_setting *layer_id = zmk_custom_setting_find("test", "layer_id");
    if (!layer_id) {
        LOG_ERR("Test layer ID setting not registered");
        return -ENOENT;
    }

    ret = zmk_custom_setting_write(layer_id, &ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(layer_id, &ZMK_CUSTOM_SETTING_VALUE_INT32(ZMK_KEYMAP_LAYERS_LEN),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected layer ID validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_validate_layer_id");

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    const struct zmk_custom_setting *behavior_id = zmk_custom_setting_find("test", "behavior_id");
    if (!behavior_id) {
        LOG_ERR("Test behavior ID setting not registered");
        return -ENOENT;
    }

    zmk_behavior_local_id_t key_press_id = zmk_behavior_get_local_id("key_press");
    if (key_press_id == UINT16_MAX) {
        LOG_ERR("Test key press behavior local ID not registered");
        return -ENOENT;
    }

    ret = zmk_custom_setting_write(behavior_id, &ZMK_CUSTOM_SETTING_VALUE_INT32(key_press_id),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(behavior_id, &ZMK_CUSTOM_SETTING_VALUE_INT32(UINT16_MAX),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected behavior ID validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_validate_behavior_id");
#endif

    return 0;
}

static int test_bytes_rpc_converters(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "bytes_value");
    if (!setting) {
        LOG_ERR("Test custom bytes setting not registered");
        return -ENOENT;
    }

    struct zmk_custom_setting_value internal_value;
    int ret = zmk_custom_setting_read(setting, &internal_value);
    if (ret < 0) {
        return ret;
    }
    const uint8_t expected_internal_default[] = {1, 2, 3};
    ret = expect_bytes_value(&internal_value, expected_internal_default,
                             sizeof(expected_internal_default));
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value rpc_value;
    ret = zmk_custom_setting_serialize_rpc_value(setting, &internal_value, &rpc_value);
    if (ret < 0) {
        return ret;
    }
    const uint8_t expected_rpc_default[] = {3, 2, 1};
    ret = expect_bytes_value(&rpc_value, expected_rpc_default, sizeof(expected_rpc_default));
    if (ret < 0) {
        return ret;
    }

    const struct zmk_custom_setting_value rpc_write = ZMK_CUSTOM_SETTING_VALUE_BYTES(9, 8, 7);
    ret = zmk_custom_setting_deserialize_rpc_value(setting, &rpc_write, &internal_value);
    if (ret < 0) {
        return ret;
    }
    const uint8_t expected_internal_write[] = {7, 8, 9};
    ret = expect_bytes_value(&internal_value, expected_internal_write,
                             sizeof(expected_internal_write));
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_bytes_rpc_convert rpc=030201 internal=070809");
    return 0;
}

static int test_scalar_lifecycle(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "int_value");
    if (!setting) {
        LOG_ERR("Test custom setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Reset custom setting unexpectedly has unsaved value");
        return -EINVAL;
    }

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(25),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value_by_key("test", "int_value", 25);
    if (ret < 0) {
        return ret;
    }

    if (!zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Memory-updated custom setting should have unsaved value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_update_read scalar=25");

    ret = zmk_custom_setting_discard(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Discarded custom setting unexpectedly has unsaved value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_discard scalar=10");

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(30),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_save(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 30);
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_has_unsaved_value(setting)) {
        LOG_ERR("Saved custom setting unexpectedly has unsaved value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_save scalar=30");

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(40),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 30);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_load scalar=30");

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(41),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_discard(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 30);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_discard_persistent scalar=30");

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_reset scalar=10");

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(101),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected range validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_validate_range");

    return 0;
}

static int test_array_lifecycle(void) {
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array_element("test", "array_value", 1);
    if (!array_setting || !zmk_custom_setting_is_array(array_setting)) {
        LOG_ERR("Test custom array setting not registered");
        return -ENOENT;
    }
    const struct zmk_custom_setting *array_tail_setting =
        zmk_custom_setting_find_array_element("test", "array_value", 2);
    if (!array_tail_setting || !zmk_custom_setting_is_array(array_tail_setting)) {
        LOG_ERR("Test custom array tail setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(array_setting);
    if (ret < 0) {
        return ret;
    }

    if (zmk_custom_setting_array_size(array_setting) != 3 ||
        zmk_custom_setting_array_max_size(array_setting) != 3) {
        LOG_ERR("Unexpected default custom array size");
        return -EINVAL;
    }

    ret = expect_int_value(array_setting, 2);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(array_tail_setting, 3);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write_array_element(array_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(55),
                                                 2, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 1, 55);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_read_array_by_key("test", "array_value", 2,
                                               &(struct zmk_custom_setting_value){0});
    if (ret != -ENOENT) {
        LOG_ERR("Expected inactive custom array element read to fail, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_update_read array[1]=55 size=2");

    ret = zmk_custom_setting_save(array_setting);
    if (ret < 0) {
        return ret;
    }
    if (!test_settings_has_record("custom_settings/test/array_value/_size") ||
        test_settings_has_record("custom_settings/test/array_value/2")) {
        LOG_ERR("Unexpected persisted custom array records");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_save_required_array_items size=2");

    ret = zmk_custom_setting_write_array_element(array_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(66),
                                                 2, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_discard(array_setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 1, 55);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 2) {
        LOG_ERR("Discard did not restore persisted custom array size");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_save_discard array[1]=55 size=2");

    ret = zmk_custom_setting_write_array_element(array_tail_setting,
                                                 &ZMK_CUSTOM_SETTING_VALUE_INT32(77), 3,
                                                 ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 2, 77);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_grow_array array[2]=77 size=3");

    uint32_t affected_count = 0;
    ret = zmk_custom_settings_reset_scope("test", "array_value", NULL, &affected_count);
    if (ret < 0) {
        return ret;
    }
    if (affected_count != 3) {
        LOG_ERR("Unexpected custom array reset affected count: %u", affected_count);
        return -EINVAL;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 1, 2);
    if (ret < 0) {
        return ret;
    }

    ret = expect_array_int_value_by_key("test", "array_value", 2, 3);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_reset array[1]=2 size=3");

    struct zmk_custom_setting_value popped_value;
    ret = zmk_custom_setting_array_pop_back(array_setting, &popped_value,
                                            ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    if (popped_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || popped_value.int32_value != 3 ||
        zmk_custom_setting_array_size(array_setting) != 2) {
        LOG_ERR("Unexpected custom array pop_back result");
        return -EINVAL;
    }
    ret = zmk_custom_setting_read_array_by_key("test", "array_value", 2,
                                               &(struct zmk_custom_setting_value){0});
    if (ret != -ENOENT) {
        LOG_ERR("Expected popped custom array element read to fail, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_pop_back array[2]=3 size=2");

    ret = zmk_custom_setting_array_push_back(array_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(88),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = expect_array_int_value_by_key("test", "array_value", 2, 88);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_array_size(array_setting) != 3) {
        LOG_ERR("Custom array push_back did not grow active size");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_push_back array[2]=88 size=3");

    ret = zmk_custom_setting_save(array_setting);
    if (ret < 0) {
        return ret;
    }
    if (!test_settings_has_record("custom_settings/test/array_value/0") ||
        !test_settings_has_record("custom_settings/test/array_value/1") ||
        !test_settings_has_record("custom_settings/test/array_value/2")) {
        LOG_ERR("Custom array save did not persist all active values");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_save_all_active_array_items size=3");

    ret = zmk_custom_setting_array_push_back(array_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(99),
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected full custom array push_back to fail, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_push_back_full size=3");
    return 0;
}

static void capture_int32_visitor(const struct zmk_custom_setting_value *value, void *user_data) {
    *(int32_t *)user_data = value->int32_value;
}

static int test_view_based_api(void) {
    const struct zmk_custom_setting *int_setting = zmk_custom_setting_find("test", "int_value");
    const struct zmk_custom_setting *bytes_setting = zmk_custom_setting_find("test", "bytes_value");
    if (!int_setting || !bytes_setting) {
        LOG_ERR("View API test settings not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(bytes_setting);
    if (ret < 0) {
        return ret;
    }

    /* zmk_custom_setting_with_value: zero-copy borrow under the lock. */
    int32_t seen = 0;
    ret = zmk_custom_setting_with_value(int_setting, capture_int32_visitor, &seen);
    if (ret < 0) {
        return ret;
    }
    if (seen != 10) {
        LOG_ERR("with_value visitor saw %d, expected default 10", seen);
        return -EINVAL;
    }

    /* zmk_custom_setting_get_int32 / set_int32. */
    int32_t int_value;
    ret = zmk_custom_setting_get_int32(int_setting, &int_value);
    if (ret < 0 || int_value != 10) {
        LOG_ERR("get_int32 returned ret=%d value=%d, expected 10", ret, int_value);
        return -EINVAL;
    }
    ret = zmk_custom_setting_set_int32(int_setting, 42, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_get_int32(int_setting, &int_value);
    if (ret < 0 || int_value != 42) {
        LOG_ERR("get_int32 after set_int32 returned ret=%d value=%d, expected 42", ret, int_value);
        return -EINVAL;
    }
    /* Type mismatch must be rejected rather than silently reinterpreted. */
    ret = zmk_custom_setting_get_int32(bytes_setting, &int_value);
    if (ret != -EINVAL) {
        LOG_ERR("Expected get_int32 on a bytes setting to fail, got %d", ret);
        return -EINVAL;
    }

    /* zmk_custom_setting_read_into with a right-sized buffer. */
    uint8_t small_buf[3];
    size_t out_size = 0;
    enum zmk_custom_setting_value_type out_type;
    ret = zmk_custom_setting_read_into(bytes_setting, small_buf, sizeof(small_buf), &out_size,
                                       &out_type);
    if (ret < 0 || out_size != 3 || out_type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
        small_buf[0] != 1 || small_buf[1] != 2 || small_buf[2] != 3) {
        LOG_ERR("read_into returned ret=%d size=%u type=%d, expected default bytes {1,2,3}", ret,
                (unsigned)out_size, out_type);
        return -EINVAL;
    }
    uint8_t too_small[2];
    ret = zmk_custom_setting_read_into(bytes_setting, too_small, sizeof(too_small), NULL, NULL);
    if (ret != -EMSGSIZE) {
        LOG_ERR("Expected read_into with an undersized buffer to fail, got %d", ret);
        return -EINVAL;
    }

    /* zmk_custom_setting_write_bytes from a runtime buffer (not a
     * compile-time-literal ZMK_CUSTOM_SETTING_VALUE_BYTES argument list). */
    uint8_t runtime_bytes[] = {5, 6, 7, 8};
    ret = zmk_custom_setting_write_bytes(bytes_setting, runtime_bytes, sizeof(runtime_bytes),
                                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    uint8_t readback[4];
    ret = zmk_custom_setting_read_into(bytes_setting, readback, sizeof(readback), &out_size, NULL);
    if (ret < 0 || out_size != 4 || memcmp(readback, runtime_bytes, sizeof(runtime_bytes)) != 0) {
        LOG_ERR("read_into after write_bytes did not return the written value");
        return -EINVAL;
    }

    ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(bytes_setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_view_api int=42 bytes=05060708");
    return 0;
}

/* Exercises the shared temporary-override pool (CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS /
 * CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE) that replaced a per-setting
 * temporary_value copy. Assumes the default pool size of 2 slots; none of
 * the test configs override it. */
static int test_temporary_override_pool(void) {
    const struct zmk_custom_setting *int_setting = zmk_custom_setting_find("test", "int_value");
    const struct zmk_custom_setting *bytes_setting = zmk_custom_setting_find("test", "bytes_value");
    const struct zmk_custom_setting *array_0 =
        zmk_custom_setting_find_array_element("test", "array_value", 0);
    const struct zmk_custom_setting *array_1 =
        zmk_custom_setting_find_array_element("test", "array_value", 1);
    if (!int_setting || !bytes_setting || !array_0 || !array_1) {
        LOG_ERR("Temporary override pool test settings not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }

    /* A temporary override is visible via read() but does not change
     * memory_value, and has_unsaved_value reports it. */
    ret = zmk_custom_setting_write(int_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(99),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 99);
    if (ret < 0) {
        return ret;
    }
    if (!zmk_custom_setting_has_unsaved_value(int_setting)) {
        LOG_ERR("Temporary override should report an unsaved value");
        return -EINVAL;
    }

    ret = zmk_custom_setting_rollback_temporary(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 10);
    if (ret < 0) {
        return ret;
    }
    if (zmk_custom_setting_has_unsaved_value(int_setting)) {
        LOG_ERR("Rolled-back custom setting unexpectedly has unsaved value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_temporary_rollback scalar=10");

    /* A value larger than one pool slot cannot use temporary mode. */
    uint8_t oversized[CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE + 1];
    memset(oversized, 0xAA, sizeof(oversized));
    struct zmk_custom_setting_value oversized_value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .size = sizeof(oversized),
    };
    memcpy(oversized_value.bytes_value, oversized, sizeof(oversized));
    ret = zmk_custom_setting_write(bytes_setting, &oversized_value,
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret != -EMSGSIZE) {
        LOG_ERR("Expected an oversized temporary write to fail with -EMSGSIZE, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_temporary_oversized size=%u", (unsigned)sizeof(oversized));

    /* Pool exhaustion: only CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS settings
     * may have an active temporary override at once. */
    ret = zmk_custom_setting_write(int_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(1),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_write(array_0, &ZMK_CUSTOM_SETTING_VALUE_INT32(2),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_write(array_1, &ZMK_CUSTOM_SETTING_VALUE_INT32(3),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret != -EBUSY) {
        LOG_ERR("Expected the temporary override pool to be exhausted, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_temporary_pool_exhausted");

    /* Freeing a slot lets a new override through. */
    ret = zmk_custom_setting_rollback_temporary(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_write(array_1, &ZMK_CUSTOM_SETTING_VALUE_INT32(3),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY);
    if (ret < 0) {
        return ret;
    }
    LOG_INF("PASS: custom_settings_temporary_pool_reclaimed");

    ret = zmk_custom_setting_rollback_temporary(array_0);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_rollback_temporary(array_1);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/* Simulates what a boot-time devicetree default installer does: replace a
 * setting's compile-time default before any user value has been set, then
 * confirm reset() re-applies the new default. */
static int test_boot_default_override(void) {
    const struct zmk_custom_setting *int_setting = zmk_custom_setting_find("test", "int_value");
    if (!int_setting) {
        return -ENODEV;
    }

    int ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }

    /* An out-of-range default is rejected and must not disturb the existing
     * default. */
    ret = zmk_custom_setting_set_default(int_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(500));
    if (ret != -ERANGE) {
        LOG_ERR("Expected out-of-range default to fail with -ERANGE, got %d", ret);
        return -EINVAL;
    }
    ret = expect_int_value(int_setting, 10);
    if (ret < 0) {
        return ret;
    }

    static const struct zmk_custom_setting_value boot_default = ZMK_CUSTOM_SETTING_VALUE_INT32(77);
    ret = zmk_custom_setting_set_default(int_setting, &boot_default);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 77);
    if (ret < 0) {
        return ret;
    }

    /* A user write still shadows the new default, and reset() restores it. */
    ret = zmk_custom_setting_write(int_setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(42),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 42);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }
    ret = expect_int_value(int_setting, 77);
    if (ret < 0) {
        return ret;
    }

    /* Restore the compile-time default so this test is order-independent. */
    ret = zmk_custom_setting_set_default(int_setting, &test_int_setting_default);
    if (ret < 0) {
        return ret;
    }
    ret = zmk_custom_setting_reset(int_setting);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("PASS: custom_settings_boot_default_override");
    return 0;
}

static int test_record_settings(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "record_value");
    if (!setting) {
        LOG_ERR("Record test setting not registered");
        return -ENOENT;
    }

    int ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    /* Nothing stored yet (default is just the TLV version byte), so
     * caller-provided defaults must survive record_get untouched. */
    struct test_profile profile = {.speed = 5, .invert = false, .label = "def"};
    ret = zmk_custom_setting_record_get(setting, &test_profile_schema, &profile);
    if (ret < 0) {
        return ret;
    }
    if (profile.speed != 5 || profile.invert != false || strcmp(profile.label, "def") != 0) {
        LOG_ERR("record_get on the default value changed caller-provided defaults");
        return -EINVAL;
    }

    struct test_profile write_profile = {.speed = 42, .invert = true, .label = "abcdefg"};
    ret = zmk_custom_setting_record_set(setting, &test_profile_schema, &write_profile,
                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    struct test_profile read_profile = {0};
    ret = zmk_custom_setting_record_get(setting, &test_profile_schema, &read_profile);
    if (ret < 0) {
        return ret;
    }
    if (read_profile.speed != 42 || read_profile.invert != true ||
        strcmp(read_profile.label, "abcdefg") != 0) {
        LOG_ERR("Record round-trip mismatch: speed=%d invert=%d label=%s", read_profile.speed,
                read_profile.invert, read_profile.label);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_record speed=42 invert=1 label=abcdefg");

    /* A field constraint violation is rejected before anything is written. */
    struct test_profile invalid_profile = {.speed = 999, .invert = false, .label = "x"};
    ret = zmk_custom_setting_record_set(setting, &test_profile_schema, &invalid_profile,
                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected an out-of-range record field to fail with -ERANGE, got %d", ret);
        return -EINVAL;
    }
    read_profile = (struct test_profile){0};
    ret = zmk_custom_setting_record_get(setting, &test_profile_schema, &read_profile);
    if (ret < 0 || read_profile.speed != 42) {
        LOG_ERR("Rejected record write unexpectedly changed the stored value");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_record_constraint_rejected");

    /* Schema evolution: decoding with a schema missing a field the data
     * has (label) skips it and leaves the destination struct's own value
     * for that field untouched. */
    uint8_t encoded[32];
    size_t encoded_size;
    ret = zmk_custom_setting_record_encode(&test_profile_schema, &write_profile, encoded,
                                           sizeof(encoded), &encoded_size);
    if (ret < 0) {
        return ret;
    }
    static const struct zmk_custom_setting_record_field speed_only_fields[] = {
        {.field_id = 1,
         .offset = offsetof(struct test_profile, speed),
         .size = sizeof(int32_t),
         .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
         .constraint = NULL},
    };
    static const struct zmk_custom_setting_record_schema speed_only_schema = {
        .fields = speed_only_fields,
        .fields_count = ARRAY_SIZE(speed_only_fields),
        .record_size = sizeof(struct test_profile),
    };
    struct test_profile speed_only = {.speed = -1, .invert = false, .label = "kept"};
    ret = zmk_custom_setting_record_decode(&speed_only_schema, encoded, encoded_size, &speed_only);
    if (ret < 0 || speed_only.speed != 42 || strcmp(speed_only.label, "kept") != 0) {
        LOG_ERR("Decoding with a narrower schema did not skip the unknown field correctly");
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_record_schema_evolution speed=42 label=kept");

    ret = zmk_custom_setting_reset(setting);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
static int test_behavior_value_type(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "behavior_value");
    if (!setting) {
        LOG_ERR("Test behavior value setting not registered");
        return -ENOENT;
    }

    /* ZMK's own "behavior" settings subtree assigns real local IDs from its
     * commit handler (behavior_local_id_init for the CRC16 id type, or
     * behavior_handle_commit for the settings-table id type); until that
     * subtree is loaded/committed once, every behavior's local_id is stuck
     * at its zero-initialized default, making zmk_behavior_get_local_id and
     * zmk_behavior_find_behavior_name_from_local_id both resolve to id 0 and
     * therefore ambiguous between behaviors. Load it explicitly here since
     * this test file's fake settings backend is otherwise only driven by
     * this module's own "custom_settings" subtree. */
    int ret = settings_load_subtree("behavior");
    if (ret < 0) {
        return ret;
    }

    zmk_behavior_local_id_t key_press_id = zmk_behavior_get_local_id("key_press");
    if (key_press_id == UINT16_MAX) {
        LOG_ERR("Test key press behavior local ID not registered");
        return -ENOENT;
    }

    /* &kp's metadata only accepts an HID_USAGE-shaped param1 and requires
     * param2 == 0, so this exercises real ZMK behavior metadata validation
     * (zmk_behavior_validate_binding), not just a local id lookup. */
    struct zmk_custom_setting_behavior_value key_a = {
        .behavior_id = key_press_id,
        .param1 = ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_A),
        .param2 = 0,
    };

    ret = zmk_custom_setting_set_behavior(setting, key_a, ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    if (ret < 0) {
        return ret;
    }

    ret = expect_behavior_value(setting, key_a);
    if (ret < 0) {
        return ret;
    }

    /* Force a reload from persistent storage to prove the varint-packed
     * storage encoding (see encode_behavior_value in custom_settings.c)
     * round-trips through the settings backend, not just the in-RAM cache. */
    ret = settings_load_subtree("custom_settings");
    if (ret < 0) {
        return ret;
    }

    ret = expect_behavior_value(setting, key_a);
    if (ret < 0) {
        return ret;
    }
    /* behavior_id is a runtime-assigned local id (registration-order
     * dependent), so it is deliberately omitted from this log line to keep
     * it deterministic across builds -- see custom_settings_validate_behavior_id
     * above for the same reasoning. */
    LOG_INF("PASS: custom_settings_behavior_value param1=0x%05x", key_a.param1);

    struct zmk_custom_setting_behavior_value bad_param1 = {
        .behavior_id = key_press_id,
        .param1 = 0xffff,
        .param2 = 0,
    };
    ret =
        zmk_custom_setting_set_behavior(setting, bad_param1, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -EINVAL) {
        LOG_ERR("Expected behavior param metadata validation failure, got %d", ret);
        return -EINVAL;
    }

    struct zmk_custom_setting_behavior_value bad_id = {
        .behavior_id = UINT16_MAX,
        .param1 = 0,
        .param2 = 0,
    };
    ret = zmk_custom_setting_set_behavior(setting, bad_id, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected behavior id validation failure, got %d", ret);
        return -EINVAL;
    }
    LOG_INF("PASS: custom_settings_behavior_value_rejects_invalid");

    return zmk_custom_setting_reset(setting);
}
#endif

static int custom_settings_test_init(void) {
    int ret = test_settings_backend_init();
    if (ret < 0) {
        return ret;
    }

    ret = test_constraint_validation();
    if (ret < 0) {
        return ret;
    }

    ret = test_bytes_rpc_converters();
    if (ret < 0) {
        return ret;
    }

    ret = test_scalar_lifecycle();
    if (ret < 0) {
        return ret;
    }

    ret = test_array_lifecycle();
    if (ret < 0) {
        return ret;
    }

    ret = test_view_based_api();
    if (ret < 0) {
        return ret;
    }

    ret = test_temporary_override_pool();
    if (ret < 0) {
        return ret;
    }

    ret = test_boot_default_override();
    if (ret < 0) {
        return ret;
    }

    ret = test_record_settings();
    if (ret < 0) {
        return ret;
    }

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
    return test_behavior_value_type();
#else
    return 0;
#endif
}

SYS_INIT(custom_settings_test_init, APPLICATION, 99);
