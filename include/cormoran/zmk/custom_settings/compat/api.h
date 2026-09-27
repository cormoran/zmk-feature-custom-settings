/* SPDX-License-Identifier: MIT */
#pragma once
#include <cormoran/zmk/custom_settings.h>
#ifndef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
#error "Legacy settings APIs require CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT"
#endif
/* Borrowed cache entries may be recycled by a later lookup. Prefer ref_find(). */
const struct zmk_custom_setting *
zmk_custom_setting_find_array_element(const char *subsystem, const char *key, uint32_t index);
/*
 * Replace a setting's default value with one resolved at boot time (e.g. from
 * devicetree data that needs behavior local IDs or other runtime-only state
 * to encode). `value` must point to storage that outlives the setting -
 * typically a caller-owned static/BSS-resident struct - since only the
 * pointer is stored, not a copy.
 *
 * Also refreshes the effective (memory) value to match, but only if no
 * persistent value has been loaded and no write has happened yet - so this
 * is safe to call from any SYS_INIT regardless of ordering relative to this
 * module's own registry init, as long as it runs before settings_load()
 * (which always happens later, from main()). Once a persisted or written
 * value exists, this only changes what a later save/discard/reset falls
 * back to.
 */
int zmk_custom_setting_set_default(const struct zmk_custom_setting *setting,
                                   const struct zmk_custom_setting_value *value);
#define ZMK_CUSTOM_SETTING_LEGACY_DEFAULTS_INIT(defaults_)                                         \
    .defaults_are_carriers = sizeof((defaults_)[0]) == sizeof(struct zmk_custom_setting_value),
