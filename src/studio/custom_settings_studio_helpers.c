/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/iterable_sections.h>
#include <zmk/studio/custom.h>

#include <cormoran/zmk/custom_settings_studio.h>

static union {
    uint64_t alignment;
    uint8_t data[CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RESPONSE_BUFFER_SIZE];
} shared_response_buffer;
static const pb_msgdesc_t *shared_response_fields;
static uintptr_t shared_response_generation;

static bool encode_shared_response(pb_ostream_t *stream, const pb_field_t *field,
                                   void *const *arg) {
    if ((uintptr_t)*arg != shared_response_generation) {
        return false;
    }
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, shared_response_fields, shared_response_buffer.data);
}

void *zmk_custom_studio_response_allocate(pb_callback_t *encode_response,
                                          const pb_msgdesc_t *response_fields,
                                          size_t response_size) {
    if (encode_response == NULL || response_fields == NULL ||
        response_size > sizeof(shared_response_buffer.data)) {
        return NULL;
    }

    memset(shared_response_buffer.data, 0, response_size);
    shared_response_fields = response_fields;
    shared_response_generation++;
    if (shared_response_generation == 0) {
        shared_response_generation++;
    }
    encode_response->funcs.encode = encode_shared_response;
    encode_response->arg = (void *)(uintptr_t)shared_response_generation;
    return shared_response_buffer.data;
}

int zmk_custom_studio_subsystem_index(const char *identifier, uint8_t *index) {
    if (identifier == NULL || index == NULL) {
        return -EINVAL;
    }

    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);

    for (size_t i = 0; i < count; i++) {
        struct zmk_rpc_custom_subsystem *subsystem;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsystem);
        if (strcmp(subsystem->identifier, identifier) == 0) {
            if (i > UINT8_MAX) {
                return -EOVERFLOW;
            }
            *index = (uint8_t)i;
            return 0;
        }
    }

    return -ENOENT;
}

int zmk_custom_studio_notify(const char *identifier,
                             zmk_custom_studio_payload_encoder_t encoder, void *arg) {
    uint8_t index;
    int ret = zmk_custom_studio_subsystem_index(identifier, &index);
    if (ret < 0) {
        return ret;
    }

    return zmk_custom_studio_notify_index(index, encoder, arg);
}

int zmk_custom_studio_notify_index(uint8_t index,
                                   zmk_custom_studio_payload_encoder_t encoder, void *arg) {
    if (encoder == NULL) {
        return -EINVAL;
    }

    return raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
        .subsystem_index = index,
        .encode_payload = {.funcs.encode = encoder, .arg = arg},
    });
}

struct notification_message_context {
    const pb_msgdesc_t *fields;
    const void *message;
};

static bool encode_notification_message(pb_ostream_t *stream, const pb_field_t *field,
                                        void *const *arg) {
    const struct notification_message_context *ctx = *arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(stream, field, ctx->fields,
                                                            ctx->message);
}

int zmk_custom_studio_notify_message(const char *identifier,
                                     const pb_msgdesc_t *message_fields,
                                     const void *message) {
    uint8_t index;
    int ret = zmk_custom_studio_subsystem_index(identifier, &index);
    if (ret < 0) {
        return ret;
    }
    return zmk_custom_studio_notify_message_index(index, message_fields, message);
}

int zmk_custom_studio_notify_message_index(uint8_t index,
                                           const pb_msgdesc_t *message_fields,
                                           const void *message) {
    if (message_fields == NULL || message == NULL) {
        return -EINVAL;
    }
    struct notification_message_context ctx = {
        .fields = message_fields,
        .message = message,
    };
    return zmk_custom_studio_notify_index(index, encode_notification_message, &ctx);
}
