/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

#include <cormoran/zmk/custom_settings/pool.h>

/* List order is address order. A grow removes its old node, compacts the
 * remaining list once, and appends its replacement. Deletes leave holes. */
static void unlink_blob(struct zmk_custom_setting_large_pool *pool,
                        struct zmk_custom_setting_blob *blob) {
    struct zmk_custom_setting_blob **link = &pool->members;
    while (*link && *link != blob) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = blob->next;
    }
    blob->next = NULL;
}

void custom_settings_pool_release(struct zmk_custom_setting_large_pool *pool,
                                  struct zmk_custom_setting_blob *blob) {
    if (blob->extent) {
        unlink_blob(pool, blob);
    }
    *blob = (struct zmk_custom_setting_blob){0};
}

size_t custom_settings_pool_used(const struct zmk_custom_setting_large_pool *pool) {
    size_t used = 0;
    for (const struct zmk_custom_setting_blob *node = pool->members; node; node = node->next) {
        used += node->extent;
    }
    return used;
}

int custom_settings_pool_reserve(struct zmk_custom_setting_large_pool *pool,
                                 struct zmk_custom_setting_blob *blob, size_t needed) {
    if (!pool || !blob || pool->size >= UINT16_MAX || needed > pool->size) {
        return -ENOSPC;
    }
    if (!needed) {
        custom_settings_pool_release(pool, blob);
        return 0;
    }
    if (needed <= blob->extent) {
        blob->extent = needed;
        return 0;
    }

    size_t others = custom_settings_pool_used(pool) - blob->extent;
    if (needed > pool->size - others) {
        return -ENOSPC;
    }

    if (blob->extent) {
        unlink_blob(pool, blob);
    }
    uint8_t *cursor = pool->data;
    struct zmk_custom_setting_blob **tail = &pool->members;
    for (struct zmk_custom_setting_blob *node = pool->members; node; node = node->next) {
        memmove(cursor, node->data, node->extent);
        node->data = cursor;
        cursor += node->extent;
        tail = &node->next;
    }
    blob->data = cursor;
    blob->extent = needed;
    blob->next = NULL;
    *tail = blob;
    return 0;
}

static struct zmk_custom_setting_blob *swapped(struct zmk_custom_setting_blob *node,
                                               struct zmk_custom_setting_blob *a,
                                               struct zmk_custom_setting_blob *b) {
    return node == a ? b : node == b ? a : node;
}

void custom_settings_pool_swap(struct zmk_custom_setting_large_pool *pool,
                               struct zmk_custom_setting_blob *a,
                               struct zmk_custom_setting_blob *b) {
    if (a == b) {
        return;
    }
    for (struct zmk_custom_setting_blob *node = pool->members; node;) {
        struct zmk_custom_setting_blob *next = node->next;
        node->next = swapped(next, a, b);
        node = next;
    }
    pool->members = swapped(pool->members, a, b);
    struct zmk_custom_setting_blob saved = *a;
    *a = *b;
    *b = saved;
}
