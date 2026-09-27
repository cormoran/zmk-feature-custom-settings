/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include "../custom_settings_internal.h"

int zmk_custom_setting_set_default(const struct zmk_custom_setting *setting,
                                   const struct zmk_custom_setting_value *value) {
    if (!setting || !value) {
        return -EINVAL;
    }

    /* Array elements have per-index defaults from array_state->defaults,
     * set once at registration time (ZMK_CUSTOM_SETTING_ARRAY_DEFINE); there
     * is no runtime-replaceable default for an array element/view. */
    if (zmk_custom_setting_is_array(setting)) {
        return -ENOTSUP;
    }

    int ret = zmk_custom_setting_validate(setting, value);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    /* The descriptor (and its default_value pointer) is const/flash-resident,
     * so the replacement default lives on the RAM state block instead and
     * setting_default_value() consults it first everywhere a default is read
     * (init, discard, reset). */
    setting->state->default_override = value;
    /* No persisted value has been loaded and no in-memory write has happened
     * yet, so the current memory value (whatever it was materialized to, or
     * even its zero-initialized pre-init state) still represents "unset" -
     * refresh it too. This makes the call safe regardless of whether it runs
     * before or after this module's own registry init, as long as it is
     * before settings_load() (i.e. from any SYS_INIT). */
    if (!(setting->state->flags &
          (ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT | ZMK_CUSTOM_SETTING_STATE_DIRTY))) {
        apply_scalar_default_locked(setting);
    }
    k_mutex_unlock(&custom_settings_lock);

    return 0;
}
