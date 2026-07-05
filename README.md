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
CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=2048
```

`CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE` must be large enough for custom settings
RPC requests.
`CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE` should be increased because listing
settings builds encoded notifications from the low priority workqueue.
When Studio RPC is enabled, setting values are limited to 64 bytes and setting
keys are limited to 48 bytes by the generated RPC schema.

### Split Keyboards

For split keyboards, enable ZMK's relay-event transport on both halves:

```conf
CONFIG_ZMK_SPLIT_RELAY_EVENT=y
CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY=y
# Optionally increase relay event data max length
# CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=256
```

Only the central half needs `CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y`. Split
peripherals can leave Studio and custom settings Studio RPC disabled; enabling
`CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY` is enough to build the protobuf
relay handler.

The custom settings relay payload size is
`CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN - 2`; the two overhead bytes store the
source and encoded payload size. Larger keys, setting values, or metadata-heavy
list responses may need a larger BLE MTU and split relay event data size.

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

| Value type                             | Default value helper                     | Description                                                                                            |
| -------------------------------------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES`  | `ZMK_CUSTOM_SETTING_VALUE_BYTES(...)`    | Raw bytes. Use this for binary data or for module-defined data that needs custom RPC conversion hooks. |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32`  | `ZMK_CUSTOM_SETTING_VALUE_INT32(value)`  | Signed 32-bit integer. Use this for numeric settings, indexes, and IDs.                                |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL`   | `ZMK_CUSTOM_SETTING_VALUE_BOOL(value)`   | Boolean setting.                                                                                       |
| `ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING` | `ZMK_CUSTOM_SETTING_VALUE_STRING(value)` | UTF-8/string setting stored with an explicit byte length.                                              |

#### Confidentiality

| Confidentiality                                     | RPC behavior                                                                                      | Typical use                                                                         |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_DEVICE_PRIVATE` | Value is not exposed over RPC.                                                                    | Device-local secrets or implementation details that Studio clients should not read. |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PERSONAL`   | Value may be read over RPC, but clients should treat it as personal data and avoid publishing it. | User-specific preferences or values that may identify the user's setup.             |
| `ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC`     | Value may be read over RPC and exported/shared by clients.                                        | Layout, behavior, or tuning settings intended to be portable.                       |

#### Constraints

| Constraint helper or type                                        | Applies to      | Description                                                                                                                               |
| ---------------------------------------------------------------- | --------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `ZMK_CUSTOM_SETTING_NO_CONSTRAINT`                               | Any type        | No validation beyond matching the registered value type.                                                                                  |
| `ZMK_CUSTOM_SETTING_RANGE_INT32(min, max)`                       | `INT32`         | Requires the integer value to be between `min` and `max`, inclusive.                                                                      |
| `ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS`                          | Any scalar type | Requires the value to match one of the provided `struct zmk_custom_setting_options` entries. Optional labels are exposed in RPC metadata. |
| `ZMK_CUSTOM_SETTING_HID_USAGE(usage_page, usage_min, usage_max)` | `INT32`         | Requires an encoded HID usage whose page matches `usage_page` and whose usage is within the inclusive range.                              |
| `ZMK_CUSTOM_SETTING_LAYER_ID`                                    | `INT32`         | Requires a valid local ZMK layer ID.                                                                                                      |
| `ZMK_CUSTOM_SETTING_BEHAVIOR_ID`                                 | `INT32`         | Requires a valid local ZMK behavior ID when behavior local IDs are enabled.                                                               |

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

### Record Settings (Structs)

A record setting stores a C struct as a single `BYTES` setting, using a
versioned, per-field TLV (tag, length, value) encoding declared through a
field schema. Unlike a hand-written bytes converter, individual fields are
validated separately, and the encoding tolerates schema evolution: a field
added to the struct later does not invalidate previously stored data (it is
just absent from old records, so the caller's own default for that field
applies), and a field removed from the schema is silently skipped when
decoding old data.

```c
struct my_profile {
    int32_t speed;
    bool invert;
    char label[12];
};

ZMK_CUSTOM_SETTING_RECORD_RANGE_INT32_DEFINE(my_profile_speed_range, 0, 100);

ZMK_CUSTOM_SETTING_RECORD_SCHEMA_DEFINE(
    my_profile_schema, struct my_profile,
    ZMK_CUSTOM_SETTING_RECORD_FIELD_INT32(struct my_profile, speed, /* field id */ 1,
                                          &my_profile_speed_range),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_BOOL(struct my_profile, invert, /* field id */ 2),
    ZMK_CUSTOM_SETTING_RECORD_FIELD_STRING(struct my_profile, label, /* field id */ 3));

ZMK_CUSTOM_SETTING_DEFINE(my_profile_setting, "my_module", "profile",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, ZMK_CUSTOM_SETTING_VALUE_BYTES(1),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

struct my_profile profile = {.speed = 50, .invert = false, .label = "default"};
zmk_custom_setting_record_get(my_profile_setting, &my_profile_schema, &profile);

profile.speed = 80;
int ret = zmk_custom_setting_record_set(my_profile_setting, &my_profile_schema, &profile,
                                        ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
/* ret == -ERANGE if a field like speed violates its constraint; nothing is
 * written in that case. */
```

Field ids are the stable identifiers for schema evolution, so keep them
fixed once a field ships (like setting keys). `_id` values are single bytes
(0-255) and only need to be unique within one schema.

Field constraints are limited to none and a range on `INT32` fields
(`zmk_custom_setting_record_set` returns `-ENOTSUP` for other constraint
kinds on a field). There is no dedicated Studio RPC message for records yet:
a record setting's RPC representation is its encoded bytes, so a Studio
client sees an opaque blob rather than a generic per-field edit form.

### Studio RPC Access

For RPC access, the setting namespace should match a Studio custom subsystem
identifier registered by the module that owns the setting. The web UI obtains
that subsystem's index from ZMK Studio and sends the index in setting requests.
The custom Studio subsystem identifier for this module is
`cormoran_custom_settings`.

### Reading And Writing Large Values In Chunks

`CONFIG_ZMK_CUSTOM_SETTINGS_CHUNKED_RPC` (enabled by default alongside Studio
RPC) adds `ReadValueChunk`/`WriteValueChunk` requests for bytes/string
settings. Each response or request carries one chunk (bounded by the
protobuf schema, independent of `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`),
so a value can be moved across several RPC frames instead of needing to fit
alongside its metadata in a single `CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE` frame:

- Read: send `ReadValueChunkRequest{setting, offset}` starting at `offset =
  0` and keep increasing `offset` by the returned chunk length until the
  response reports `last = true`. Reads are stateless; chunks may be
  re-read or read out of order.
- Write: send `WriteValueChunkRequest{setting, total_size, offset, data,
  commit, mode}` starting at `offset = 0`. Chunks for one transfer must
  arrive in order; the value is validated and applied only when a chunk
  sets `commit = true`, so a partially-sent value is never observable by
  readers. Only one write transfer is staged at a time
  (`CONFIG_ZMK_CUSTOM_SETTINGS_CHUNK_STAGING_SIZE`); starting a new
  transfer (`offset = 0`) for any setting replaces an abandoned one.

### Array Settings

Array settings are registered with a **single macro call** for the whole array:
one descriptor owns one contiguous backing buffer sized for the maximum
element count, instead of one registration per element. The active array
length can be smaller than the maximum. The firmware storage key is expanded
to `key/index` (plus a `key/_size` entry for the active length), but the
public API and RPC protocol use the base key plus an explicit array index and
active length. Flash storage saves the active length and only the active
array items. RPC does not expose the maximum length; appending past it
returns an error.

```c
/* Declare the per-index defaults first (a plain macro argument works for
 * short literals like ZMK_CUSTOM_SETTING_VALUE_INT32(0) elsewhere in this
 * guide, but an array of defaults needs its own declaration so it can be
 * passed as a pointer - see the comment on
 * ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE for why). */
ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE(my_layers_default, 0, 1, 2);

/* One registration for up to 3 elements, with active length starting at 3. */
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(my_layers, "my_module", "layers",
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                /* max_count = */ 3, /* default_size = */ 3,
                                my_layers_default,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_RANGE_INT32(0, 31));
```

`ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_CONSTRAINTS` and
`ZMK_CUSTOM_SETTING_ARRAY_DEFINE_WITH_RPC_CONVERTERS[_AND_CONSTRAINTS]`
follow the same naming pattern as the scalar `ZMK_CUSTOM_SETTING_DEFINE*`
macros for zero-or-more constraints and/or custom bytes RPC converters.

> **Migrating from `ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE`**: older versions
> of this module registered arrays one element at a time
> (`ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE(name, subsys, key, index,
> max_size, ...)` called once per index, each with its own default). That
> macro has been **removed**, not kept as a compatibility shim: independent
> per-index macro expansions (e.g. a `LISTIFY`/`UTIL_LISTIFY` loop) have no
> way to share the one contiguous buffer the new design requires, so there is
> no mechanical way to keep the old call sites compiling. Replace each array's
> N old registrations with one `ZMK_CUSTOM_SETTING_ARRAY_DEFINE` call, moving
> each element's old default into one `ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_*`
> declaration. The on-flash storage format is unchanged (`key/index` and
> `key/_size` entries persist and load exactly as before), so existing user
> data survives the migration with no extra code.

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

/* Insert/remove in the middle of the array. Both shift the remaining active
 * elements over the array's single contiguous buffer, so they are cheap
 * regardless of array length. */
zmk_custom_setting_array_insert_at(layers, 1, &ZMK_CUSTOM_SETTING_VALUE_INT32(9),
                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
struct zmk_custom_setting_value removed;
zmk_custom_setting_array_remove_at(layers, 1, &removed, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
```

`struct zmk_custom_setting_value` is sized by
`CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`, so declaring one on the stack
just to read or write a small value, or to write a byte buffer whose size
isn't known at compile time, is wasteful or impossible with the macros
above. A view-based API avoids both:

```c
/* Read into a right-sized buffer instead of a full zmk_custom_setting_value. */
int32_t speed;
zmk_custom_setting_get_int32(setting, &speed);
zmk_custom_setting_set_int32(setting, 20, ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

/* zmk_custom_setting_get_bool/set_bool follow the same pattern. */

/* Write bytes/string settings from a runtime buffer of dynamic length. */
uint8_t payload[dynamic_len];
zmk_custom_setting_write_bytes(blob_setting, payload, dynamic_len,
                               ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);

/* Copy any setting's raw payload into a caller-sized buffer; fails with
 * -EMSGSIZE instead of overflowing if the buffer is too small. */
uint8_t buf[8];
size_t size;
zmk_custom_setting_read_into(setting, buf, sizeof(buf), &size, NULL);

/* Borrow the effective value with no copy at all; the callback runs while
 * the settings lock is held, so keep it short and do not call back into
 * other zmk_custom_setting_* functions from within it. */
void log_speed(const struct zmk_custom_setting_value *value, void *user_data) {
    ARG_UNUSED(user_data);
    LOG_INF("speed=%d", value->int32_value);
}
zmk_custom_setting_with_value(setting, log_speed, NULL);
```

### Memory Notes

Each setting keeps only its current in-memory value in RAM; the default
value lives in flash and is referenced by pointer, and the persisted value is
not cached separately:

- `ZMK_CUSTOM_SETTING_WRITE_MODE_TEMPORARY` overrides come from a small
  shared pool instead of a dedicated slot per setting
  (`CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS`, default 2;
  `CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOT_SIZE`, default 32 bytes). Writing a
  value larger than the slot size in temporary mode fails with `-EMSGSIZE`;
  writing while the pool is full fails with `-EBUSY`. Other write modes are
  unaffected by either limit.
- `zmk_custom_setting_discard` re-reads the previously saved value from flash
  instead of restoring a RAM-cached copy. This is a rare, explicit user
  action, so the flash read is not a concern; `zmk_custom_setting_has_unsaved_value`
  stays a cheap flag check (it does not read flash) and reports "written
  since the last save/discard/reset", which also means writing back the
  already-saved value still reports unsaved until the next save/discard.

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
