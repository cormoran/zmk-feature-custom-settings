/* SPDX-License-Identifier: MIT */
#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/custom_settings/ref.h>

#ifndef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
BUILD_ASSERT(sizeof(void *) != 4 || sizeof(struct zmk_custom_setting) == 40,
             "Compact ARM descriptor size changed");
BUILD_ASSERT(sizeof(void *) != 4 || sizeof(struct zmk_custom_setting_state) == 1,
             "Compact state must not contain default_override");
#endif
#define PUBLIC ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC
#define OPEN ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE
ZMK_CUSTOM_SETTING_DEFINE(compact_number, "compact", "number", ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                          ZMK_CUSTOM_SETTING_VALUE_INT32(5), PUBLIC, OPEN, OPEN,
                          ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_DEFINE(compact_text, "compact", "text", ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                          ZMK_CUSTOM_SETTING_VALUE_STRING("hello"), PUBLIC, OPEN, OPEN,
                          ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
/* Existing consumers may take the address of this legacy byte field. */
static const uint8_t *const legacy_permission_field __unused = &compact_number.confidentiality;
BUILD_ASSERT(sizeof(void *) != 4 || sizeof(struct zmk_custom_setting) == 52);
#endif
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
static const int32_t defaults[] = {1, 2, 3};
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(compact_array, "compact", "array",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, 3, 3, defaults, PUBLIC, OPEN,
                                OPEN, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#endif

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_KEYSPACE)
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(compact_keys, "compact", "entry/",
                                   ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, sizeof(int32_t), 16, 3,
                                   PUBLIC, OPEN, OPEN, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#endif

#include <zephyr/init.h>
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS_ARRAY)
/* Same lookup/read in both fixtures. The old consumer activates its cache;
 * the migrated consumer keeps only a copyable identity. */
static int sample_read(void) {
    int32_t number;
#ifdef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array_element("compact", "array", 0);
    return zmk_custom_setting_get_int32(setting, &number);
#else
    struct zmk_custom_setting_ref ref;
    int ret = zmk_custom_setting_ref_find("compact", "array", 0, &ref);
    return ret ? ret : zmk_custom_setting_ref_read_into(&ref, &number, sizeof(number), NULL, NULL);
#endif
}
SYS_INIT(sample_read, APPLICATION, 99);
#endif

BUILD_ASSERT(sizeof(void *) != 4 || sizeof(struct zmk_custom_setting_value_view) == 8,
             "Core values must not embed a maximum-size payload buffer");
#ifndef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
BUILD_ASSERT(sizeof(void *) != 4 || sizeof(struct zmk_custom_setting_value) == 8,
             "Compact public values use caller-owned payload storage");
#endif

BUILD_ASSERT(offsetof(struct zmk_custom_setting_value_view, size) == 2);
BUILD_ASSERT(sizeof(void *) != 4 ||
             offsetof(struct zmk_custom_setting_value_view, int32_value) == 4);

#ifndef CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_COMPAT
BUILD_ASSERT(sizeof(void *) != 4 || sizeof(struct zmk_custom_setting_int32_state) == 8);
BUILD_ASSERT(sizeof(struct zmk_custom_setting_bool_state) == 2);
BUILD_ASSERT(sizeof(void *) != 4 || sizeof(struct zmk_custom_setting_behavior_state) == 16);
BUILD_ASSERT(sizeof(void *) != 4 || sizeof(struct zmk_custom_setting_blob_state) == 16);
#endif
