/* SPDX-License-Identifier: MIT */
#pragma once

#include <cormoran/zmk/custom_settings.h>

/* All helpers require custom_settings_lock. Flags describe ordinal storage
 * records, so shifting values must not shift has_persistent bits. */
static inline bool array_flag_get(const uint8_t *bits, uint32_t index) {
    return (bits[index / 8] & (1U << (index % 8))) != 0;
}
static inline void array_flag_set(uint8_t *bits, uint32_t index, bool value) {
    uint8_t mask = 1U << (index % 8);
    bits[index / 8] = (bits[index / 8] & ~mask) | (value ? mask : 0);
}

void array_value_read(const struct zmk_custom_setting *array, uint32_t index,
                      struct zmk_custom_setting_value_view *value);
int array_value_write(const struct zmk_custom_setting *array, uint32_t index,
                      const struct zmk_custom_setting_value_view *value);
void array_value_default(const struct zmk_custom_setting *array, uint32_t index);
void array_value_swap(const struct zmk_custom_setting *array, uint32_t a, uint32_t b);
