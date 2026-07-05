# Design Proposal: Flexibility and Memory Redesign

Status: proposal (not implemented)

This document analyzes the current limitations of `zmk-feature-custom-settings`
and proposes a redesign that makes the module more generic and flexible for
downstream ZMK modules, including modules generated from
`zmk-module-template-with-custom-studio-rpc` which recommends this module as
the default settings mechanism.

## Goals

1. Allow large setting values without inflating the RAM cost of every other
   setting and without risking stack overflow.
2. Make arrays — especially arrays of structs — easy to declare, cheap in RAM,
   and editable from the web UI.
3. Support keys that are not fixed at compile time (driver instances,
   user-created entries).
4. Keep the flash storage schema compatible so existing user data survives
   upgrades wherever possible.

## Current Design and Its Costs

### Value storage

`struct zmk_custom_setting_value` embeds a union sized by the global
`CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` (default 64):

```c
struct zmk_custom_setting_value {
    enum zmk_custom_setting_value_type type;  /* 4 bytes */
    size_t size;                              /* 4 bytes */
    union {
        uint8_t bytes_value[VALUE_MAX_SIZE];      /* 64 */
        int32_t int32_value;
        bool bool_value;
        char string_value[VALUE_MAX_SIZE + 1];    /* 65 -> padded 68 */
    };
};                                            /* = 76 bytes at max 64 */
```

Every registered `struct zmk_custom_setting` holds **four** copies
(`default_value`, `persistent_value`, `memory_value`, `temporary_value`) plus
metadata, and the whole struct is mutable, so it lives in RAM:

| Item                                 | RAM at max=64 | RAM at max=256 |
| ------------------------------------ | ------------- | -------------- |
| One `zmk_custom_setting_value`       | ~76 B         | ~268 B         |
| One setting (any type, even `bool`)  | ~372 B        | ~1.2 KB        |
| `int32[16]` array (16 registrations) | ~5.9 KB       | ~19 KB         |

Consequences:

- A `bool` setting costs ~372 B of RAM to store 1 byte of payload.
- Raising `VALUE_MAX_SIZE` for one large blob multiplies the cost of **every**
  setting and every stack-allocated `zmk_custom_setting_value` local. The RPC
  handler keeps several value locals on the stack
  (`handle_write_setting` → `proto_to_value` → `scalar_proto_to_value`, plus
  `internal_value` for RPC deserialization), so max=256 easily adds >1 KB of
  stack per request on the low-priority work queue
  (`CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE` default 2048). This is the
  observed stack-overflow / wasted-memory trade-off.
- `copy_value()` always copies the full union regardless of the actual value
  size.

### Arrays

Arrays are registered one element at a time
(`ZMK_CUSTOM_SETTING_ARRAY_ELEMENT_DEFINE` × N). Each element is a complete
`zmk_custom_setting` with its own four value copies, and per-array state
(`array_size`, `persistent_array_size`) is duplicated into every element and
kept in sync by iterating the whole registry
(`set_array_memory_size_locked`). Structs inside arrays require
`VALUE_TYPE_BYTES` plus hand-written RPC converter hooks, and the web UI can
only show them as raw bytes.

### Keys

Registration is only possible through `STRUCT_SECTION_ITERABLE` macros with
string literals, checked by `BUILD_ASSERT(sizeof(...))`. There is no runtime
registration path, so:

- a driver with a devicetree-instance-dependent number of channels cannot
  register per-instance settings without preprocessor tricks;
- users cannot create entries (named profiles, macros, mappings) from the web
  UI at all.

### RPC limits

`custom_settings.options` pins `bytes_value`/`string_value` to
`max_size:64` and keys to 48 bytes. nanopb generates fixed-size arrays from
these, so the request/response/notification statics and all intermediate
copies scale with the limit. There is no way to move a value larger than one
RPC frame.

## Proposals

The proposals are ordered so that P1–P3 form the core redesign; P4–P6 build on
them. A phasing plan follows at the end.

### P1. Split ROM descriptor from RAM state; per-setting value capacity

Replace the single mutable `zmk_custom_setting` with:

```c
/* Constant part, placed in flash (const iterable section). */
struct zmk_custom_setting_desc {
    const char *custom_subsystem_id;
    const char *key;
    enum zmk_custom_setting_value_type value_type;
    enum zmk_custom_setting_confidentiality confidentiality;
    enum zmk_custom_setting_permission read_permission;
    enum zmk_custom_setting_permission write_permission;
    const struct zmk_custom_setting_constraint *constraints;
    uint16_t constraints_count;

    /* Value geometry */
    uint16_t max_size;        /* capacity of one element               */
    uint16_t max_count;       /* 1 for scalars, N for arrays (P3)      */
    const void *default_data; /* default payload in flash              */
    uint16_t default_size;    /* per element                           */
    uint16_t default_count;   /* initial array length                  */

    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer;
    zmk_custom_setting_rpc_bytes_converter_t rpc_deserializer;

    /* RAM side */
    struct zmk_custom_setting_state *state; /* small, fixed size     */
    uint8_t *data;                          /* max_size * max_count  */
};

/* Mutable part, RAM. sizes[] needed only when elements vary in size. */
struct zmk_custom_setting_state {
    uint16_t count;      /* active array length (1 for scalars) */
    uint8_t flags;       /* DIRTY | HAS_PERSISTENT | TEMP_ACTIVE */
    uint16_t *sizes;     /* actual size per element (bytes/string) */
};
```

The `ZMK_CUSTOM_SETTING_DEFINE` macro allocates the state and a backing buffer
sized for the actual setting, not the global maximum:

```c
#define ZMK_CUSTOM_SETTING_DEFINE(_name, _subsys, _key, _type, _max_size, ...) \
    static uint8_t _name##_data[_max_size];                                    \
    static uint16_t _name##_sizes[1];                                          \
    static struct zmk_custom_setting_state _name##_state = {...};              \
    static const STRUCT_SECTION_ITERABLE(zmk_custom_setting_desc, _name) = {...}
```

Key changes compared to today:

- **`default_value` stays in flash.** The descriptor points at a `const`
  payload; nothing is copied to RAM at init.
- **Drop the always-resident `persistent_value` copy.** Track a `DIRTY` flag
  instead: writes set it, save/load clear it. `discard()` re-reads the value
  from flash via `settings_load_subtree("custom_settings/<subsys>/<key>")`
  (falling back to the default when nothing is stored). Discard is a rare,
  user-initiated operation, so a flash read is acceptable.
  - Semantic note: `has_unsaved_value` becomes "written since last
    save/load" — writing the identical value back now reports dirty. This is
    acceptable for the UI (it already refreshes on save/discard) and is much
    cheaper than keeping a full shadow copy.
- **Move `temporary_value` out of the per-setting struct.** Temporary
  overrides are rare and short-lived; keep a small shared pool
  (`CONFIG_ZMK_CUSTOM_SETTINGS_TEMP_SLOTS`, default 2–4 slots of the largest
  registered size, or per-slot fixed capacity). A setting with an active
  override points at its slot. `-EBUSY` when the pool is exhausted.
- `CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE` remains only as the default for
  macros that do not pass an explicit `max_size`, and as the transport-side
  cap (P6). It no longer sizes any struct.

Expected RAM per setting (descriptor moves to flash):

| Case                          | Current (max=64)   | Proposed                 |
| ----------------------------- | ------------------ | ------------------------ |
| `bool`                        | ~372 B             | ~12 B                    |
| `int32`                       | ~372 B             | ~16 B                    |
| 64 B bytes blob               | ~372 B             | ~80 B                    |
| `int32[16]` array             | ~5.9 KB            | ~90 B                    |
| 512 B blob                    | impossible (>64)   | ~530 B (only this one)   |

### P2. Zero-copy value access API (stack safety)

Replace pass-by-value unions with views and explicit buffers:

```c
struct zmk_custom_setting_data {
    enum zmk_custom_setting_value_type type;
    size_t size;
    const void *data;
};

/* Copy out into a caller-sized buffer. */
int zmk_custom_setting_read_into(const struct zmk_custom_setting_desc *s,
                                 void *buf, size_t capacity, size_t *size);

/* Borrow under the registry lock — no copy at all. */
int zmk_custom_setting_with_value(const struct zmk_custom_setting_desc *s,
                                  void (*fn)(const struct zmk_custom_setting_data *d,
                                             void *user_data),
                                  void *user_data);

/* Writes take pointer + length; validation happens against the view. */
int zmk_custom_setting_write(const struct zmk_custom_setting_desc *s,
                             const void *data, size_t size,
                             enum zmk_custom_setting_write_mode mode);

/* Typed convenience wrappers (compile down to the above). */
int zmk_custom_setting_get_int32(const struct zmk_custom_setting_desc *s, int32_t *v);
int zmk_custom_setting_set_int32(const struct zmk_custom_setting_desc *s, int32_t v,
                                 enum zmk_custom_setting_write_mode mode);
/* ... bool / string variants ... */
```

RPC handler changes:

- Encode values directly from the setting's backing buffer under the lock
  using nanopb **callback fields** (`pb_callback_t`) for `bytes_value` /
  `string_value` instead of fixed `max_size` arrays. The generated
  request/response/notification structs shrink to pointers and no longer
  scale with the value limit.
- Decode writes through a callback into a single staging buffer (also used by
  chunked transfer, P6) instead of a stack-resident scalar struct.

Result: stack usage of the RPC path becomes O(1) with respect to value size,
and `CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE` guidance can be relaxed.

Migration: keep the current `zmk_custom_setting_value`-based functions as
thin deprecated wrappers (behind `CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_API`,
default `y` for one release) that stack-allocate only up to a small legacy
cap, so existing modules compile during the transition.

### P3. First-class arrays: one registration, one buffer

Replace per-element registration with a single macro:

```c
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(
    my_layers, "my_module", "layers",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    /* elem max_size = */ sizeof(int32_t),
    /* max_count      = */ 16,
    /* default        = */ ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32(0, 1, 2),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    ZMK_CUSTOM_SETTING_RANGE_INT32(0, 31));
```

- One descriptor + one contiguous `max_size × max_count` buffer + one `count`.
  Constraints apply to every element. Optional per-index defaults come from
  `default_data`/`default_count`.
- The O(N²) size-synchronization (`set_array_memory_size_locked` walking all
  registrations per write) disappears; `count` lives in one place.
- Element API stays: `read_at(s, idx, ...)`, `write_at(s, idx, ...)`,
  `push_back`, `pop_back`, plus new `insert_at`/`remove_at` (now trivial with
  a contiguous buffer — today they would require shifting values across
  registrations).

**Storage schema: unchanged.** Elements persist as
`custom_settings/<subsys>/<key>/<index>` and the active length as
`.../<key>/_size`, so existing flash data loads into the new layout without
migration. Per-element entries also keep individual writes small, which is
friendly to NVS/ZMS.

RPC additions (backward compatible):

```proto
// Read/notify a contiguous range of elements in one message.
message SettingArrayValues {
    uint32 start = 1;
    uint32 size = 2;            // active array length
    repeated SettingScalarValue values = 3;  // callback-encoded
}
```

`SettingValue.array_value` (single element) remains valid; list responses use
`SettingArrayValues` so a 16-element array is one notification instead of 16
notifications spaced 10 ms apart.

### P4. Record settings: structs with a declared field schema

Today a struct value means `BYTES` + hand-written converters and an opaque hex
blob in the UI. Add a declarative field table:

```c
struct my_profile {
    int32_t speed;
    bool invert;
    char label[12];
};

ZMK_CUSTOM_SETTING_RECORD_FIELDS(my_profile_fields,
    ZMK_CS_FIELD_INT32(struct my_profile, speed, "speed",
                       ZMK_CUSTOM_SETTING_RANGE_INT32(0, 100)),
    ZMK_CS_FIELD_BOOL(struct my_profile, invert, "invert"),
    ZMK_CS_FIELD_STRING(struct my_profile, label, "label"));

ZMK_CUSTOM_SETTING_RECORD_ARRAY_DEFINE(
    my_profiles, "my_module", "profiles",
    struct my_profile, my_profile_fields,
    /* max_count = */ 8, /* defaults... */ ...);
```

- The core validates writes **per field** (each field reuses the existing
  constraint machinery) and exposes the schema over RPC so the web UI renders
  a generic form (number input, checkbox, text box) instead of raw bytes.
- Wire format: a tiny field-tagged TLV (`field_id, wire_type, len, payload`)
  rather than the raw C struct. This makes the format independent of
  compiler padding and **versionable**: unknown fields are skipped, missing
  fields keep their defaults, so adding a field to a struct does not
  invalidate stored data. A record version byte prefixes the TLV.
- Firmware access is typed and copy-based:
  `zmk_custom_setting_record_get(&my_profiles, idx, &profile)` /
  `..._set(...)` generated as inline wrappers by the macro.
- The existing serializer/deserializer hooks remain the escape hatch for
  fully custom formats.

Proto additions:

```proto
message SettingFieldMeta {
    uint32 field_id = 1;
    string name = 2;
    SettingScalarType type = 3;
    repeated SettingConstraint constraints = 4;
}
// SettingMeta gains: repeated SettingFieldMeta fields = 5;  (callback-encoded)
// Record values travel as bytes_value containing the TLV encoding; the UI
// decodes/encodes using the field metadata.
```

### P5. Runtime registration and dynamic keyspaces

Two levels, both heap-free:

**(a) Runtime registration with caller-owned storage.** For drivers whose
setting count depends on devicetree or runtime probing:

```c
int zmk_custom_settings_register(struct zmk_custom_setting_desc *desc);
```

- Descriptors registered this way go on a linked list; every iteration point
  (`find`, list RPC, settings load handler) walks section + list.
- If registration happens after `settings_load()`, the module calls
  `settings_load_subtree()` for that key so the persisted value is applied.
- The descriptor, state, and buffer are owned by the caller (static or
  DT-macro-generated per instance), so no heap is involved.

**(b) RPC-creatable keyspaces.** For user-created entries (profiles, named
mappings) a module registers a keyspace with a slot pool instead of fixed
keys:

```c
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE(
    my_macros, "my_module", /* key prefix = */ "macro/",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, /* max_size = */ 128,
    /* max_entries = */ 8, /* permissions, confidentiality, constraints */ ...);
```

- New RPC requests `CreateSetting { subsystem, key, value }` /
  `DeleteSetting { ref }` allocate/release a slot when the key matches a
  registered keyspace prefix and the quota allows it. Key names occupy a
  fixed `KEY_MAX_LEN` buffer per slot.
- Persisted entries under the prefix are re-bound to slots during settings
  load, so user-created keys survive reboot without any module code.
- Firmware discovers entries via the existing `key_prefix` filter and a
  change-event subscription.

### P6. Large values: callback encoding + chunked transfer

With P1/P2 the firmware side supports arbitrary per-setting sizes. The
remaining caps are transport-level (RPC frame, RX buffer, split relay
payload). Two complementary measures:

1. **Callback-encoded proto fields** (P2) decouple generated struct sizes
   from value limits. Raise the practical single-frame limit to what
   `CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE` allows, and validate at runtime
   instead of via `max_size` codegen.
2. **Chunked read/write RPC** for values larger than one frame:

```proto
message ReadValueChunkRequest  { SettingRef setting = 1; uint32 offset = 2; }
message ValueChunkResponse     { uint32 total_size = 1; uint32 offset = 2;
                                 bytes data = 3; bool last = 4; }
message WriteValueChunkRequest { SettingRef setting = 1; uint32 total_size = 2;
                                 uint32 offset = 3; bytes data = 4;
                                 bool commit = 5; SettingWriteMode mode = 6; }
```

- Writes stage into one global staging buffer
  (`CONFIG_ZMK_CUSTOM_SETTINGS_STAGING_SIZE`, allocated only when chunked
  RPC is enabled). Validation and the actual `write()` run on `commit`, so a
  half-transferred value is never observable. A timeout releases an
  abandoned staging session.
- Chunk size follows the transport: min(RX buffer, relay payload) — the split
  relay path keeps working without raising
  `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN`.
- The web UI hides chunking entirely behind its read/write helpers.

### P7. Smaller improvements

- **Batch list notifications**: pack multiple `Setting` items per
  notification frame (callback-encoded repeated field) instead of one item
  per 10 ms tick; large registries currently take seconds to list.
- Demote the `LOG_INF` calls in `setting_to_proto` / list handling to
  `LOG_DBG`; they run per item on the hot path.
- Do flash writes for `PERSIST` mode from the low-priority work queue instead
  of while holding `settings_lock` in the caller's context, so a slow flash
  erase cannot stall readers on other threads.
- Hide the mutable state from the public header (`struct
  zmk_custom_setting_state` becomes private); today every internal field is
  part of the de-facto API, which makes evolution breaking by default.
- Add `insert_at` / `remove_at` array operations (cheap after P3).

## Compatibility and Migration

| Area              | Impact                                                                                                                                              |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| Flash storage     | **No change.** Same subtree, same per-element keys, same `_size` entries. Existing user data loads into the new layout.                              |
| Firmware API      | Breaking at the source level (descriptor type and value API change). Legacy wrappers behind `CONFIG_ZMK_CUSTOM_SETTINGS_LEGACY_API` for one release. |
| Registration      | `ZMK_CUSTOM_SETTING_DEFINE` keeps its shape plus a new `max_size` argument; array element macros are replaced by `ZMK_CUSTOM_SETTING_ARRAY_DEFINE`.   |
| RPC proto         | Additive only: new messages/fields, existing tags unchanged. Old web UIs keep working against new firmware for existing features.                    |
| Web UI            | Ships with the module; updated in lockstep (ranged arrays, record forms, chunked transfer, create/delete).                                           |

## Phasing

1. **Phase 1 — non-breaking groundwork**
   - P7 logging + batched list notifications.
   - P6 chunked RPC (additive) and callback-encoded proto fields.
   - P2 view-based read/write API added alongside the existing API.
2. **Phase 2 — core redesign (major version bump)**
   - P1 descriptor/state split, per-setting capacity, dirty-flag persistence.
   - P3 single-registration arrays.
   - Legacy API shims; template repository and README migration guide updated.
3. **Phase 3 — new capabilities**
   - P4 record settings with field schema and generic UI forms.
   - P5 runtime registration, then RPC-creatable keyspaces.

Each phase follows the repository rule: proto definition → firmware handler →
web UI, with unit tests in `tests/` and build coverage in `tests/zmk-config`.

## Open Questions

1. Should `discard` on a dirty setting re-read flash (proposed) or should
   settings opt in to keeping a shadow copy when instant discard matters?
2. Temporary-override pool sizing: fixed slot capacity (simple) vs.
   largest-registered-size slots (no failure mode for big settings)?
3. For record arrays, is TLV the right wire format, or should records reuse
   nanopb with a module-supplied `.proto` (more tooling, but heavier)?
4. Keyspace slot keys consume `KEY_MAX_LEN` RAM per slot; is a tighter
   per-keyspace key length worth the extra Kconfig knob?
