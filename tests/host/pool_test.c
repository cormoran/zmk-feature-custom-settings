/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <cormoran/zmk/custom_settings/pool.h>

static unsigned seed = 19;
static unsigned random_number(void) {
    seed = seed * 1664525U + 1013904223U;
    return seed >> 8; /* Low LCG bits repeat with the call count. */
}

int main(void) {
    uint8_t bytes[127] = {0};
    struct zmk_custom_setting_large_pool pool = {.data = bytes, .size = sizeof(bytes)};
    struct zmk_custom_setting_blob nodes[17] = {0};
    uint8_t expected[17][40] = {{0}};
    size_t lengths[17] = {0};
    unsigned operations[4] = {0};
    for (unsigned iteration = 0; iteration < 20000; iteration++) {
        unsigned index = random_number() % 17;
        unsigned other = random_number() % 17;
        unsigned length = random_number() % 40;
        unsigned operation = random_number() % 4;
        operations[operation]++;
        if (operation == 0) {
            custom_settings_pool_release(&pool, &nodes[index]);
            lengths[index] = 0;
        } else if (operation == 1) {
            custom_settings_pool_swap(&pool, &nodes[index], &nodes[other]);
            uint8_t saved[40];
            memcpy(saved, expected[index], sizeof(saved));
            memcpy(expected[index], expected[other], sizeof(saved));
            memcpy(expected[other], saved, sizeof(saved));
            size_t old_length = lengths[index];
            lengths[index] = lengths[other];
            lengths[other] = old_length;
        } else {
            uint8_t before[sizeof(bytes)];
            struct zmk_custom_setting_blob nodes_before[17];
            memcpy(before, bytes, sizeof(bytes));
            memcpy(nodes_before, nodes, sizeof(nodes));
            size_t used = custom_settings_pool_used(&pool);
            int ret = custom_settings_pool_reserve(&pool, &nodes[index], length);
            if (used - lengths[index] + length > sizeof(bytes)) {
                assert(ret == -ENOSPC);
                assert(memcmp(before, bytes, sizeof(bytes)) == 0);
                assert(memcmp(nodes_before, nodes, sizeof(nodes)) == 0);
            } else {
                assert(ret == 0);
                lengths[index] = length;
                nodes[index].size = length;
                memset(expected[index], iteration % 251, length);
                if (length) {
                    memcpy(nodes[index].data, expected[index], length);
                }
            }
        }
        size_t used = 0;
        unsigned count = 0;
        const uint8_t *end = pool.data;
        for (struct zmk_custom_setting_blob *node = pool.members; node; node = node->next) {
            assert(++count <= 17);
            assert(node->data >= end);
            end = node->data + node->extent;
            assert(end <= bytes + sizeof(bytes));
            assert(node->extent);
            used += node->extent;
        }
        size_t expected_used = 0;
        for (unsigned i = 0; i < 17; i++) {
            assert(nodes[i].extent == lengths[i]);
            if (lengths[i]) {
                assert(memcmp(nodes[i].data, expected[i], lengths[i]) == 0);
            }
            expected_used += lengths[i];
        }
        assert(used == expected_used);
    }
    for (unsigned operation = 0; operation < 4; ++operation) {
        assert(operations[operation] > 1000);
    }
    assert(custom_settings_pool_reserve(&pool, &nodes[0], SIZE_MAX) == -ENOSPC);
    puts("pool: 20000 deterministic mutations passed");
}
