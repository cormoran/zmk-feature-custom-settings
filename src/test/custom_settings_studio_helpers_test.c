/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <pb_encode.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/custom_settings_studio.h>
#include <cormoran/zmk/custom_settings/custom_settings.pb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int custom_settings_studio_helpers_test_init(void) {
    uint8_t index;

    if (zmk_custom_studio_subsystem_index(NULL, &index) != -EINVAL ||
        zmk_custom_studio_subsystem_index("cormoran_custom_settings", NULL) != -EINVAL) {
        LOG_ERR("FAIL: custom Studio subsystem lookup accepted an invalid argument");
        return -EINVAL;
    }

    if (zmk_custom_studio_subsystem_index("cormoran_custom_settings", &index) != 0) {
        LOG_ERR("FAIL: custom Studio subsystem lookup did not find its own subsystem");
        return -ENOENT;
    }

    if (zmk_custom_studio_subsystem_index("cormoran__missing", &index) != -ENOENT) {
        LOG_ERR("FAIL: custom Studio subsystem lookup accepted an unknown identifier");
        return -EINVAL;
    }

    if (zmk_custom_studio_notify("cormoran_custom_settings", NULL, NULL) != -EINVAL ||
        zmk_custom_studio_notify_index(index, NULL, NULL) != -EINVAL ||
        zmk_custom_studio_notify_message("cormoran_custom_settings", NULL, NULL) != -EINVAL ||
        zmk_custom_studio_notify_message_index(index, NULL, NULL) != -EINVAL) {
        LOG_ERR("FAIL: custom Studio notification accepted a null encoder");
        return -EINVAL;
    }

    pb_callback_t first = {0};
    pb_callback_t second = {0};
    uint8_t *first_buffer = zmk_custom_studio_response_allocate(
        &first, cormoran_zmk_custom_settings_Response_fields,
        sizeof(cormoran_zmk_custom_settings_Response));
    if (first_buffer == NULL) {
        LOG_ERR("FAIL: shared custom Studio response allocation failed");
        return -ENOMEM;
    }
    first_buffer[0] = 0xff;

    uint8_t *second_buffer = zmk_custom_studio_response_allocate(
        &second, cormoran_zmk_custom_settings_Response_fields,
        sizeof(cormoran_zmk_custom_settings_Response));
    if (second_buffer != first_buffer || second_buffer[0] != 0) {
        LOG_ERR("FAIL: shared custom Studio response buffer was not reused and cleared");
        return -EINVAL;
    }

    pb_ostream_t sizing_stream = PB_OSTREAM_SIZING;
    void *stale_generation = first.arg;
    if (first.funcs.encode(&sizing_stream, NULL, &stale_generation)) {
        LOG_ERR("FAIL: stale shared custom Studio response encoder was accepted");
        return -EINVAL;
    }

    if (zmk_custom_studio_response_allocate(
            &second, cormoran_zmk_custom_settings_Response_fields,
            CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RESPONSE_BUFFER_SIZE + 1) != NULL) {
        LOG_ERR("FAIL: oversized shared custom Studio response allocation was accepted");
        return -EINVAL;
    }

    LOG_INF("PASS: custom_settings_studio_helpers");
    return 0;
}

SYS_INIT(custom_settings_studio_helpers_test_init, APPLICATION,
         CONFIG_APPLICATION_INIT_PRIORITY);
