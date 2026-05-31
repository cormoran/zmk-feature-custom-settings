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

### Add The Module

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

### Enable The Module

Enable the module in your keyboard config:

```conf
CONFIG_ZMK_CUSTOM_SETTINGS=y

# Optional: expose settings through the custom Studio RPC subsystem.
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_REQUEST_PAYLOAD_MAX_BYTES=96
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
```

`CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE` must be large enough for custom settings
RPC requests. `CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_REQUEST_PAYLOAD_MAX_BYTES`
must also be large enough for the encoded custom settings request payload.
`CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE` should be increased because listing
settings builds encoded notifications from the low priority workqueue.
When Studio RPC is enabled, setting values are limited to 64 bytes and setting
keys are limited to 48 bytes by the generated RPC schema.

### Split Keyboards

For split keyboards, enable ZMK's relay-event transport on both halves:

```conf
CONFIG_ZMK_SPLIT_RELAY_EVENT=y
CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY=y
```

By default, this uses ZMK's 14-byte split relay event payload and reserves 12
bytes for encoded custom settings relay payloads. Larger keys, setting values,
or metadata-heavy list responses may need a larger BLE MTU and matching relay
event/custom settings payload sizes.

### Register Settings

Register a setting from another module:

```c
#include <cormoran/zmk/custom_settings.h>

ZMK_CUSTOM_SETTING_DEFINE(
    // C symbol name for this setting registration.
    my_speed_setting,
    // Custom Studio subsystem id owned by your module.
    "my_module",
    // Setting key within that subsystem.
    "speed",
    // Value type exposed by the firmware API and RPC.
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    // Default value used when nothing is saved in flash.
    ZMK_CUSTOM_SETTING_VALUE_INT32(10),
    // RPC visibility: public, personal, or device-private.
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    // Read permission: secure requires Studio unlock.
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    // Write permission: secure requires Studio unlock.
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    // Optional validation rule; use NO_CONSTRAINT when unrestricted.
    ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100));
```

The subsystem id and key are the stable identifiers used by firmware and RPC
clients. Pick short, unique names because they are encoded into settings keys
and Studio custom subsystem requests.

### Setting Reference

#### Value Types

| Value type | Default value helper | Description |
| --- | --- | --- |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES` | `ZMK_CUSTOM_SETTING_VALUE_BYTES(...)` | Raw bytes. Use this for binary data or for module-defined data that needs custom RPC conversion hooks. |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32` | `ZMK_CUSTOM_SETTING_VALUE_INT32(value)` | Signed 32-bit integer. Use this for numeric settings, indexes, and IDs. |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL` | `ZMK_CUSTOM_SETTING_VALUE_BOOL(value)` | Boolean setting. |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING` | `ZMK_CUSTOM_SETTING_VALUE_STRING(value)` | UTF-8/string setting stored with an explicit byte length. |

#### Confidentiality

| Confidentiality | RPC behavior | Typical use |
| --- | --- | --- |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE` | Value is not exposed over RPC. | Device-local secrets or implementation details that Studio clients should not read. |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL` | Value may be read over RPC, but clients should treat it as personal data and avoid publishing it. | User-specific preferences or values that may identify the user's setup. |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC` | Value may be read over RPC and exported/shared by clients. | Layout, behavior, or tuning settings intended to be portable. |

#### Constraints

| Constraint helper or type | Applies to | Description |
| --- | --- | --- |
| `ZMK_CUSTOM_SETTING_NO_CONSTRAINT` | Any type | No validation beyond matching the registered value type. |
| `ZMK_CUSTOM_SETTING_RANGE_INT32(min, max)` | `INT32` | Requires the integer value to be between `min` and `max`, inclusive. |
| `ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS` | Any scalar type | Requires the value to match one of the provided `struct zmk_custom_setting_options` entries. Optional labels are exposed in RPC metadata. |
| `ZMK_CUSTOM_SETTING_HID_USAGE(usage_page, usage_min, usage_max)` | `INT32` | Requires an encoded HID usage whose page matches `usage_page` and whose usage is within the inclusive range. |
| `ZMK_CUSTOM_SETTING_LAYER_ID` | `INT32` | Requires a valid local ZMK layer ID. |
| `ZMK_CUSTOM_SETTING_BEHAVIOR_ID` | `INT32` | Requires a valid local ZMK behavior ID when behavior local IDs are enabled. |

### Bytes RPC Conversion

Bytes settings may define RPC serializer/deserializer hooks. Firmware APIs and
flash storage keep the internal byte format; RPC read/write uses the converted
byte format. When hooks are omitted, bytes are copied as-is.

```c
static int my_blob_to_rpc(const struct zmk_custom_setting *setting,
                          const uint8_t *src, size_t src_size,
                          uint8_t *dest, size_t *dest_size,
                          size_t dest_capacity) {
    ARG_UNUSED(setting);

    // Encode an internal C struct into RPC bytes, for example with nanopb.
    if (src_size > dest_capacity) {
        return -EMSGSIZE;
    }
    memcpy(dest, src, src_size);
    *dest_size = src_size;
    return 0;
}

static int my_blob_from_rpc(const struct zmk_custom_setting *setting,
                            const uint8_t *src, size_t src_size,
                            uint8_t *dest, size_t *dest_size,
                            size_t dest_capacity) {
    ARG_UNUSED(setting);

    // Decode RPC bytes back into the firmware's internal C struct layout.
    if (src_size > dest_capacity) {
        return -EMSGSIZE;
    }
    memcpy(dest, src, src_size);
    *dest_size = src_size;
    return 0;
}

ZMK_CUSTOM_SETTING_DEFINE_WITH_RPC_CONVERTERS(
    my_blob_setting, "my_module", "blob", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
    ZMK_CUSTOM_SETTING_VALUE_BYTES(0, 0, 0, 0),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    my_blob_to_rpc, my_blob_from_rpc,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
```

### Studio RPC Access

For RPC access, the setting namespace should match a Studio custom subsystem
identifier registered by the module that owns the setting. The web UI obtains
that subsystem's index from ZMK Studio and sends the index in setting requests.
The custom Studio subsystem identifier for this module is
`cormoran_custom_settings`.

### Array Settings

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

### Firmware API

Read or update settings from firmware:

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

## Web UI

The web UI in `web/` connects to a keyboard over serial, finds the
`cormoran_custom_settings` subsystem, and sends typed read/write/save/discard/reset
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

The `tests/zmk-config` build enables `CONFIG_ZMK_CUSTOM_SETTINGS_ZMK_CONFIG_SAMPLES`
for module-enabled artifacts. Flash `custom_settings_board_with_rpc` to a real
device to test Studio RPC with the sample custom subsystem `zmk_config_sample`.
For split testing, flash `custom_settings_split_central_with_rpc_relay` to the
central half and `custom_settings_split_peripheral_with_rpc_relay` to the
peripheral half. The sample subsystem registers int32, bool, string, bytes,
bytes-with-RPC-conversion, array, private, secure, HID usage, layer id, and
behavior id settings where supported.

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
