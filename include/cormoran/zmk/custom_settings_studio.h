/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <pb.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Find a registered custom Studio subsystem by its stable identifier. */
int zmk_custom_studio_subsystem_index(const char *identifier, uint8_t *index);

typedef bool (*zmk_custom_studio_payload_encoder_t)(pb_ostream_t *stream,
                                                    const pb_field_t *field,
                                                    void *const *arg);

/**
 * Raise a custom Studio notification for a subsystem identifier.
 *
 * The payload encoder is invoked synchronously by the current ZMK Studio
 * implementation, so arg may refer to caller-owned storage that remains valid
 * until this function returns.
 */
int zmk_custom_studio_notify(const char *identifier,
                             zmk_custom_studio_payload_encoder_t encoder, void *arg);

/** Raise a custom Studio notification when the subsystem index is already cached. */
int zmk_custom_studio_notify_index(uint8_t index,
                                   zmk_custom_studio_payload_encoder_t encoder, void *arg);

/** Raise a notification by encoding a nanopb message with the shared encoder. */
int zmk_custom_studio_notify_message(const char *identifier,
                                     const pb_msgdesc_t *message_fields,
                                     const void *message);

/** Index-based variant for high-frequency callers that cache subsystem lookup. */
int zmk_custom_studio_notify_message_index(uint8_t index,
                                           const pb_msgdesc_t *message_fields,
                                           const void *message);

/** Allocate the shared custom Studio response buffer and install its encoder. */
void *zmk_custom_studio_response_allocate(pb_callback_t *encode_response,
                                          const pb_msgdesc_t *response_fields,
                                          size_t response_size);

#define ZMK_CUSTOM_STUDIO_RESPONSE_BUFFER(response_type)                                           \
    BUILD_ASSERT(sizeof(response_type) <=                                                          \
                     CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RESPONSE_BUFFER_SIZE,                       \
                 #response_type " exceeds the shared custom Studio response buffer");             \
    BUILD_ASSERT(__alignof__(response_type) <= __alignof__(uint64_t),                              \
                 #response_type " requires stronger shared response buffer alignment")

#define ZMK_CUSTOM_STUDIO_RESPONSE_BUFFER_ALLOCATE(response_type, encode_response)                 \
    ((response_type *)zmk_custom_studio_response_allocate(                                         \
        (encode_response), response_type##_fields, sizeof(response_type)))

#ifdef __cplusplus
}
#endif
