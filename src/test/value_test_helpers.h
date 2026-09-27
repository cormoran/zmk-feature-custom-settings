/* SPDX-License-Identifier: MIT */
#pragma once
/* Regression tests reuse one output variable across scalar and blob reads.
 * Reattach its caller-owned buffer before each compact read: scalar values
 * overwrite the pointer/capacity union. Legacy carriers own their own bytes. */
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
#define TEST_VALUE_OUTPUT(name_) (&(name_))
#else
#define TEST_VALUE_OUTPUT(name_)                                                                   \
    ((name_) = (struct zmk_custom_setting_value){.bytes_value = name_##_storage,                   \
                                                 .size = sizeof(name_##_storage)},                 \
     &(name_))
#endif
