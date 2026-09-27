/* SPDX-License-Identifier: MIT */
#pragma once
/* Preserve direct field access for existing consumers. */
struct zmk_custom_setting_state {
    uint8_t flags;
    union {
        int32_t int32_value;
        bool bool_value;
        struct zmk_custom_setting_behavior_value behavior;
        struct zmk_custom_setting_blob blob;
    };
    /* Caller-owned override must outlive all uses of this setting. */
    const struct zmk_custom_setting_value *default_override;
};
