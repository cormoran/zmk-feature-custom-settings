# ZMK Custom Settings

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)

This module provides a typed custom settings registry for ZMK modules and an
unofficial custom ZMK Studio RPC interface for editing those settings from the
web UI.

## Features

- Register settings from any module with a custom subsystem namespace and key.
  RPC clients address the namespace by its ZMK Studio custom subsystem index.
- Store typed values: bytes, int32, bool, string, and dynamic arrays of
  those scalar types.
- Validate writes with optional range, option-list, HID usage, layer ID, or
  behavior ID constraints.
- Write values in memory, persist them to flash, discard memory changes, or
  reset persisted values back to defaults.
- Mark values as device-private, RPC-personal, or RPC-public.
- Require Studio unlock independently for reads and writes.
- Notify Studio clients when values change.

## Module User Guide

Add the module to `config/west.yml`. This module uses the custom Studio RPC
support from cormoran's ZMK branch.

```yml
manifest:
  remotes:
    - name: cormoran
      url-base: https://github.com/cormoran
  projects:
    - name: zmk-feature-custom-settings
      remote: cormoran
      revision: main
    - name: zmk
      remote: cormoran
      revision: main+custom-studio-protocol
      import:
        file: app/west.yml
```

Enable the module in your keyboard config:

```conf
CONFIG_ZMK_CUSTOM_SETTINGS=y

# Optional: expose settings through the custom Studio RPC subsystem.
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
```

For split keyboards, enable ZMK's relay-event transport on both halves and size
the relay event buffer for the setting notifications you expect to relay:

```conf
CONFIG_ZMK_SPLIT_RELAY_EVENT=y
CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN=4
CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=256
CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY=y
```

Register a setting from another module:

```c
#include <zmk/custom_settings.h>

ZMK_CUSTOM_SETTING_DEFINE(my_speed_setting, "my_module", "speed",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                          ZMK_CUSTOM_SETTING_VALUE_INT32(10),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                          ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));
```

For RPC access, the setting namespace should match a Studio custom subsystem
identifier registered by the module that owns the setting. The web UI obtains
that subsystem's index from ZMK Studio and sends the index in setting requests.

Array settings are registered one element at a time up to the maximum supported
length. The active array length can be smaller than the maximum. The firmware
storage key is expanded to `key/index`, but the public API and RPC protocol use
the base key plus an explicit array index and active length. Flash storage saves
the active length and only the active array items. RPC does not expose the
maximum length; appending past it returns an error.

```c
ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(my_layer_0, "my_module", "layers", 0, 3,
                                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                        ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                                        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 31));

ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(my_layer_1, "my_module", "layers", 1, 3,
                                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                        ZMK_CUSTOM_SETTING_VALUE_INT32(1),
                                        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 31));

ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(my_layer_2, "my_module", "layers", 2, 3,
                                        ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                        ZMK_CUSTOM_SETTING_VALUE_INT32(2),
                                        ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                        ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                        ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                        ZMK_CUSTOM_SETTING_RANGE_INT32(0, 31));
```

Read or update it from firmware:

```c
struct zmk_custom_setting_value value;
const struct zmk_custom_setting *setting =
    zmk_custom_setting_find("my_module", "speed");

zmk_custom_setting_read(setting, &value);
zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(20),
                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
zmk_custom_setting_save(setting);

zmk_custom_setting_write_array_by_key("my_module", "layers", 1,
                                      &ZMK_CUSTOM_SETTING_VALUE_INT32(4),
                                      ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

const struct zmk_custom_setting *layer_1 =
    zmk_custom_setting_find_array_element("my_module", "layers", 1);
zmk_custom_setting_write_array_element(layer_1, &ZMK_CUSTOM_SETTING_VALUE_INT32(4),
                                       2, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

const struct zmk_custom_setting *layers =
    zmk_custom_setting_find_array("my_module", "layers");
zmk_custom_setting_array_push_back(layers, &ZMK_CUSTOM_SETTING_VALUE_INT32(5),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
zmk_custom_setting_array_pop_back(layers, NULL, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
```

The custom Studio subsystem identifier is `zmk__custom_settings`.

## Web UI

The web UI in `web/` connects to a keyboard over serial, finds the
`zmk__custom_settings` subsystem, and sends typed read/write/save/discard/reset
requests. It can also export all RPC-readable setting values as JSON and import
that JSON back to the device using the selected write mode. Array values can be
written by index or changed with push-back/pop-back commands.

```bash
cd web
npm install
npm run dev
```

See [web/README.md](./web/README.md) for web development commands.

## Development

```bash
# Run unit test + build test and verify the results
python3 -m unittest

# Run build test directly
west zmk-build tests/zmk-config

# Run unit test directly
west zmk-test tests -m .

# Run web tests
cd web && npm test

# Run lint/test hooks before commit
pre-commit run --all-files
```
