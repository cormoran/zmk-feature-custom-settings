/* SPDX-License-Identifier: MIT */
#include "../custom_settings_internal.h"

/* Legacy pointer adapter. New retained references use owner/index/generation
 * and resolve into a caller-local view, without consuming this cache. */
struct zmk_custom_setting_array_view_slot {
    bool in_use;
    struct zmk_custom_setting view;
};

static struct zmk_custom_setting_array_view_slot
    array_view_pool[CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY_VIEW_POOL_SIZE];

/* Only this compatibility adapter owns recyclable descriptors. */
static struct zmk_custom_setting *
array_view_acquire(const struct zmk_custom_setting *array_descriptor, uint32_t index) {
    struct zmk_custom_setting_array_view_slot *free_slot = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(array_view_pool); i++) {
        struct zmk_custom_setting_array_view_slot *slot = &array_view_pool[i];
        if (slot->in_use && slot->view.array_state == array_descriptor->array_state &&
            slot->view.array_index == index) {
            return &slot->view;
        }
        if (!free_slot && !slot->in_use) {
            free_slot = slot;
        }
    }

    if (!free_slot) {
        /* Only the legacy pointer adapter is recycled. Temporary overlays
         * belong to (array, index), so eviction cannot discard a value. */
        free_slot = &array_view_pool[0];
    }

    free_slot->in_use = true;
    free_slot->view = *array_descriptor;
    free_slot->view.array_index = index;
    free_slot->view.array_state = array_descriptor->array_state;
    free_slot->view.default_value = NULL;
    free_slot->view.state = array_descriptor->state;
    return &free_slot->view;
}

const struct zmk_custom_setting *
zmk_custom_setting_find_array_element(const char *custom_subsystem_id, const char *key,
                                      uint32_t index) {
    const struct zmk_custom_setting *array_setting =
        zmk_custom_setting_find_array(custom_subsystem_id, key);
    if (!array_setting || index >= array_setting->array_state->max_size) {
        return NULL;
    }

    k_mutex_lock(&custom_settings_lock, K_FOREVER);
    struct zmk_custom_setting *view = array_view_acquire(array_setting, index);
    k_mutex_unlock(&custom_settings_lock);

    return view;
}
