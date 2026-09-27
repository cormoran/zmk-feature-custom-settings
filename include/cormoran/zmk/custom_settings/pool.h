/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* A stable node describes movable bytes. extent == 0 means a borrowed ROM
 * slice (or an empty value), which does not participate in the pool list. */
struct zmk_custom_setting_blob {
    uint8_t *data;
    struct zmk_custom_setting_blob *next;
    uint16_t size;
    uint16_t extent;
};

struct zmk_custom_setting_large_pool {
    uint8_t *data;
    size_t size;
    struct zmk_custom_setting_blob *members;
};

/* Internal allocator API. The caller serializes access and stages input outside
 * the pool before reserve. A successful reserve invalidates borrowed pointers.
 * An error leaves every node and byte unchanged. */
int custom_settings_pool_reserve(struct zmk_custom_setting_large_pool *pool,
                                 struct zmk_custom_setting_blob *blob, size_t needed);
void custom_settings_pool_release(struct zmk_custom_setting_large_pool *pool,
                                  struct zmk_custom_setting_blob *blob);
size_t custom_settings_pool_used(const struct zmk_custom_setting_large_pool *pool);
void custom_settings_pool_swap(struct zmk_custom_setting_large_pool *pool,
                               struct zmk_custom_setting_blob *a,
                               struct zmk_custom_setting_blob *b);
