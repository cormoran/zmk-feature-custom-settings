/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <cormoran/zmk/custom_settings.h>
#include <stdbool.h>

struct zmk_custom_setting;

/* A ref is a copyable identity, never a borrowed descriptor or pool pointer.
 * Array refs name an index, not the value currently occupying that index.
 * Keyspace refs also name a generation, so deletion/reuse returns ESTALE. */
struct zmk_custom_setting_ref {
    const void *owner;
    uint32_t index;
    uint32_t generation;
    enum {
        ZMK_CUSTOM_SETTING_REF_STATIC,
        ZMK_CUSTOM_SETTING_REF_ARRAY,
        ZMK_CUSTOM_SETTING_REF_KEYSPACE
    } kind;
};

typedef int (*zmk_custom_setting_ref_visitor_t)(const struct zmk_custom_setting *setting,
                                                void *context);
int zmk_custom_setting_ref_capture(const struct zmk_custom_setting *setting,
                                   struct zmk_custom_setting_ref *ref);
/* Runs under the settings lock. The descriptor is valid only during visit.
 * Visitors may call synchronous settings APIs, but must not wait for a worker
 * or retain the descriptor. Acquire outer locks before calling this API. */
int zmk_custom_setting_ref_visit(const struct zmk_custom_setting_ref *ref,
                                 zmk_custom_setting_ref_visitor_t visit, void *context);
bool zmk_custom_setting_ref_equal(const struct zmk_custom_setting_ref *a,
                                  const struct zmk_custom_setting_ref *b);

/* Resolve an identity without allocating a legacy view. Use ARRAY_NONE for a
 * scalar, keyspace key or array parent. An inactive index resolves, but read
 * fails with ENOENT until it becomes active. */
int zmk_custom_setting_ref_find(const char *subsystem, const char *key, uint32_t index,
                                struct zmk_custom_setting_ref *ref);
/* Copies into caller storage; no borrowed pool pointer crosses the lock. */
int zmk_custom_setting_ref_read_into(const struct zmk_custom_setting_ref *ref, void *buffer,
                                     size_t capacity, size_t *size,
                                     enum zmk_custom_setting_value_type *type);

/* Copies a value using the setting's declared type. Scalars use the same
 * native C representation as read_into; STRING length excludes the NUL.
 * Input is borrowed only for this call, including pool-backed inputs. */
int zmk_custom_setting_ref_write(const struct zmk_custom_setting_ref *ref, const void *data,
                                 size_t size, enum zmk_custom_setting_write_mode mode);
