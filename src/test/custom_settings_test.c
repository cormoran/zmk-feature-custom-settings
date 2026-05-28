/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <zmk/custom_settings.h>
#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

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

ZMK_CUSTOM_SETTING_DEFINE(test_int_setting, "test", "int_value",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, ZMK_CUSTOM_SETTING_VALUE_INT32(10),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(test_array_setting_0, "test", "array_value", 0, 2,
                                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                        ZMK_CUSTOM_SETTING_VALUE_INT32(1),
                                        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(test_array_setting_1, "test", "array_value", 1, 2,
                                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                        ZMK_CUSTOM_SETTING_VALUE_INT32(2),
                                        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));

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

static int custom_settings_test_init(void) {
    const struct zmk_custom_setting *setting = zmk_custom_setting_find("test", "int_value");
    if (!setting) {
        LOG_ERR("Test custom setting not registered");
        return -ENOENT;
    }

    int ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(25),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 25);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(101),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret != -ERANGE) {
        LOG_ERR("Expected range validation failure, got %d", ret);
        return -EINVAL;
    }

    ret = zmk_custom_setting_discard(setting);
    if (ret < 0) {
        return ret;
    }

    ret = expect_int_value(setting, 10);
    if (ret < 0) {
        return ret;
    }

    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array_element("test", "array_value", 1);
    if (!array_setting || !zmk_custom_setting_is_array(array_setting)) {
        LOG_ERR("Test custom array setting not registered");
        return -ENOENT;
    }

    ret = expect_int_value(array_setting, 2);
    if (ret < 0) {
        return ret;
    }

    ret = zmk_custom_setting_write_array_by_key("test", "array_value", 1,
                                                &ZMK_CUSTOM_SETTING_VALUE_INT32(55),
                                                ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        return ret;
    }

    return expect_int_value(array_setting, 55);
}

SYS_INIT(custom_settings_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
