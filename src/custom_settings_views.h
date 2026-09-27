/* SPDX-License-Identifier: MIT */
#pragma once
#include <cormoran/zmk/custom_settings.h>

/* Caller-owned descriptors: valid only in the enclosing synchronous scope. */
struct zmk_custom_setting *array_view_init(struct zmk_custom_setting *view,
                                           const struct zmk_custom_setting *array, uint32_t index);
struct zmk_custom_setting *array_view_find(struct zmk_custom_setting *view, const char *subsystem,
                                           const char *key, uint32_t index);
