# ZMK Custom Settings

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)

This module provides a typed custom settings registry for ZMK modules and an
unofficial custom ZMK Studio RPC interface for editing those settings from the
web UI.

## Features

- Register settings from any module with a subsystem namespace and key.
- Store typed values: bytes, int32, bool, and string.
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
the relay payload for the encoded protobuf messages you expect to relay:

```conf
CONFIG_ZMK_SPLIT_RELAY_EVENT=y
CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN=4
CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=224
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

Read or update it from firmware:

```c
struct zmk_custom_setting_value value;
const struct zmk_custom_setting *setting =
    zmk_custom_setting_find("my_module", "speed");

zmk_custom_setting_read(setting, &value);
zmk_custom_setting_write(setting, &ZMK_CUSTOM_SETTING_VALUE_INT32(20),
                         ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
zmk_custom_setting_save(setting);
```

The custom Studio subsystem identifier is `zmk__custom_settings`.

## Web UI

The web UI in `web/` connects to a keyboard over serial, finds the
`zmk__custom_settings` subsystem, and sends typed read/write/save/discard/reset
requests.

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
