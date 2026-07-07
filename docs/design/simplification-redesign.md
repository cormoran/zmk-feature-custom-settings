# Simplification redesign: fewer, more orthogonal primitives

Status: **proposal / design only** (no implementation yet)
Backward compatibility: **intentionally allowed to break** (wire format, storage
schema, and public macros/APIs). There is exactly one external consumer today
(`zmk-feature-runtime-macro`); its migration is spelled out in
[§8](#8-migration-for-the-only-external-consumer).

## 1. Motivation

The module accreted several overlapping mechanisms as features were added
one PR at a time (issue #12 → PRs #10/#15/#23/#24/#25/#26 and follow-ups
#16–#22). The result is large and has more than one way to do the same thing:

- `src/custom_settings.c` ≈ 3175 lines, `src/studio/custom_settings_handler.c`
  ≈ 3108 lines, `include/cormoran/zmk/custom_settings.h` ≈ 1193 lines.
- **Three** different large-value backing stores coexist
  (per-setting fixed buffer, shared pool, per-keyspace-slot buffer).
- Reading a large value has a whole dedicated request type + session state +
  staging buffer that, on closer inspection, the transport does not require.
- A general runtime-registration facility (`zmk_custom_settings_register()`)
  exists purely to support keyspaces; it forced a virtual iterator to replace
  `STRUCT_SECTION_FOREACH` at ~24 call sites.

### Who actually uses what (whole-workspace audit)

The only out-of-module consumer is `zmk-feature-runtime-macro`, and it uses
only:

- `ZMK_CUSTOM_SETTING_DEFINE`
- `ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE` + `ZMK_CUSTOM_SETTING_DEFINE_POOLED`

No external code uses `ZMK_CUSTOM_SETTING_DEFINE_SIZED`,
`ZMK_CUSTOM_SETTING_ARRAY_DEFINE`, `zmk_custom_settings_register()`, or any
keyspace API. This is the empirical basis for the cuts below.

## 2. Goals & non-goals

**Goals**

1. Collapse the three large-value backing stores into **one** (the pool).
2. Delete the read-chunk RPC and its state machine; serve any value size from
   the ordinary `GetSetting` by streaming it.
3. Delete the general runtime-registration facility and re-implement keyspaces
   in a leaner, self-contained form that does **not** need it, so
   `ZMK_CUSTOM_SETTING_FOREACH` can revert to a plain section walk.
4. Shrink per-setting RAM: split the ~160-byte RAM-resident descriptor into
   flash-resident const metadata + a ~16-byte RAM state block, and stop
   embedding a worst-case 64-byte value union in every descriptor.

**Non-goals (explicitly kept as-is this round)**

- Array settings (`ARRAY_DEFINE`, view pool, push/pop/insert/remove, the
  `SettingArrayValue` wire type).
- The macro variant matrix (`_WITH_CONSTRAINTS` / `_WITH_RPC_CONVERTERS` / …).
- `TEMPORARY` write mode and its slot pool.
- Record settings (P4) and the RPC bytes converter hooks.

These are left untouched to bound the blast radius of this redesign; they can
be revisited separately.

---

## 3. Change 1 — one large-value backing store (pools)

### 3.1 Problem

A BYTES/STRING value larger than the fixed carrier
(`CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE`, 64) can be backed three ways:

| Mechanism | Field(s) | Allocated by |
|---|---|---|
| Dedicated fixed buffer | `large_data != NULL`, `large_pool == NULL` | `DEFINE_SIZED` |
| Shared pool (on-demand region) | `large_pool != NULL`, `large_data` lazy | `DEFINE_POOLED` |
| Per-keyspace-slot buffer | keyspace `value_bufs` → slot `large_data` | `KEYSPACE_DEFINE` |

Every read/write/save/load site therefore has to reason about "which of the
three am I looking at". `setting_uses_large_store()` already had to widen to
`large_data != NULL || large_pool != NULL` to paper over the first two.

### 3.2 Observation

`DEFINE_SIZED(name, N, …)` is exactly `DEFINE_POOLED` against a private pool
of size `N`:

```c
#define ZMK_CUSTOM_SETTING_DEFINE_SIZED(_name, _max_size, ...)              \
    ZMK_CUSTOM_SETTING_LARGE_POOL_DEFINE(_name##_auto_pool, _max_size);     \
    ZMK_CUSTOM_SETTING_DEFINE_POOLED(_name, _max_size, _name##_auto_pool, ...)
```

A one-member pool never moves (compaction is a no-op), so this is behaviorally
identical to today's dedicated buffer, at the cost of the pool descriptor
(two words) and the on-demand-allocation code path — which already exists and
is exercised by `runtime-macro`.

### 3.3 Design

- Reimplement `DEFINE_SIZED` as the sugar above (or drop the name entirely —
  see §7). Either way **the dedicated-fixed-buffer path is deleted from the
  core**: no more `large_data`-set-at-init, no more "is this a SIZED or a
  POOLED" branch. `setting_uses_large_store()` becomes `large_pool != NULL`.
- Keyspaces that need large values (§5) draw their slot payloads from a pool
  too, instead of `value_bufs`. That removes the third path.

**Result:** exactly one large-value mechanism (the pool), used by SIZED-sugar,
POOLED, and large keyspaces alike. `large_data` remains only as the
"currently-resolved region pointer" that pool code re-derives under
`settings_lock`; `value_max_size`/`large_size` semantics are unchanged.

### 3.4 Make the pool own its members (prerequisite for §5)

Today `pool_ensure_region()` and `zmk_custom_setting_large_pool_used()`
discover a pool's members by walking `ZMK_CUSTOM_SETTING_FOREACH` and filtering
`large_pool == pool` (`custom_settings.c:170/185/285`). That only works while
every pooled member is reachable through the global setting iterator. Once §5
takes keyspace slots out of that iterator, a keyspace value stored in a pool
would be **missed by compaction** and its region corrupted.

Fix by giving the pool an **intrusive member list** instead of discovering
members through the global iterator:

```c
struct zmk_custom_setting_large_pool {
    uint8_t *data;
    size_t   size;
    struct zmk_custom_setting *members;  // head of intrusive list
};
// struct zmk_custom_setting gains a `pool_next` link (pool-local use only).
```

A member links into `pool->members` when it first allocates a region and
unlinks when its value shrinks to empty. Compaction and `pool_used()` walk
`pool->members`. This **decouples the pool from `ZMK_CUSTOM_SETTING_FOREACH`
entirely**, so it works for any member — a compile-time pooled setting *or* a
keyspace slot — that links in.

This is not a return of the deleted `_runtime_next` global registry list: that
list was a *global* registry that `FOREACH` had to merge, forcing virtual
iteration at ~24 sites. `pool_next` is *pool-local*, touched only by the two
pool functions in `custom_settings.c`; no other call site iterates it. The
address-lowest-first compaction is order-independent, so a linked-list
enumeration is fine.

(Alternative considered: keep iterator-based discovery but have the pool code
also walk keyspace slots. Rejected — it re-couples pool code to keyspace
iteration; the member list is self-contained and simpler.)

**Cost/caveat:** a single-member pool costs one extra descriptor vs a bare
buffer, and pooled storage moves regions on growth (invisible to callers — no
site caches `large_data` across a lock drop, an invariant we keep). Acceptable.

---

## 4. Change 2 — delete `ReadValueChunk`, stream `GetSetting`

### 4.1 Why the read side is unnecessary

Today `GetSetting` gives up on a value that exceeds the carrier (sets
`has_value = false`, see `custom_settings_handler.c:610-622`) and forces the
client to fetch it with `ReadValueChunk`. The reason is **not** the transport:
it is that the response's value lives in a **fixed inline nanopb array**
(`bytes_value.bytes[max_size]`, capped by the proto `.options` `max_size:64`).

The ZMK Studio RPC transport **streams the response incrementally** — confirmed
on hardware during the #24 validation: `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE` is a
transmit-chunk size, not a maximum message length (TX=64 served 128-byte reads
fine). So the only thing standing between `GetSetting` and an arbitrarily large
value is the inline array.

### 4.2 Design

Encode the setting value's bytes/string payload with a **nanopb callback field**
(`pb_callback_t`, i.e. a custom `encode` function) that writes directly from the
value's backing store to the output `pb_ostream_t`, instead of a fixed inline
array. Then `GetSetting` (and every list-item `Setting`) carries the full value
regardless of size, and the entire read-chunk apparatus is dead code.

- The callback fires during the framework's `pb_encode`, i.e. **after** the
  handler has returned. It therefore re-acquires `settings_lock` and re-derives
  `large_data`/`large_size` for the setting at encode time — safe because the
  module never caches those across a lock (same invariant as §3). This also
  lets us drop the read staging buffer (`chunk_read_buffer`) entirely: we stream
  straight from the backing store, no intermediate copy.
- Small values encode through the same callback from the carrier, so there is
  one value-encode path, not a "small inline vs large chunked" fork.

### 4.3 Why the write side stays

The RX path buffers a whole frame into `CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE`
**before** nanopb decodes it, so a large incoming value genuinely cannot arrive
in one frame. `WriteValueChunk` (offset-ordered, `commit`-gated) remains the
way to deliver a large value to the device. This read/write asymmetry is
inherent to "TX streams out, RX buffers in", not an oversight.

### 4.4 Deleted vs kept

**Deleted:** `ReadValueChunkRequest`, the read use of `ValueChunkResponse`,
`handle_private_read_value_chunk`, `chunk_read_buffer`, the read half of
`web/src/chunkedValue.ts`, and the read-side of the `Response.value_chunk`
plumbing.

**Kept:** `WriteValueChunkRequest`, its session state
(`chunk_session`/`chunk_session_lock`), the write staging buffer, and the write
half of `chunkedValue.ts`. `CONFIG_ZMK_CUSTOM_SETTINGS_CHUNKED_RPC` /
`CHUNK_STAGING_SIZE` remain but now gate only the write path (rename help text).

---

## 5. Change 3 — remove runtime registration; lean keyspaces

### 5.1 What is removed

- Public API `zmk_custom_settings_register()` / `zmk_custom_settings_unregister()`.
- The `_runtime_next` field on `struct zmk_custom_setting` and the
  `runtime_settings_head` list.
- The **virtual iterator** `zmk_custom_settings_foreach_first/next`.
  `ZMK_CUSTOM_SETTING_FOREACH` reverts to a plain
  `STRUCT_SECTION_FOREACH(zmk_custom_setting, …)`. The ~24 call sites that only
  ever needed compile-time settings (init, set_default, find, apply_scope, …)
  drop the indirection with no behavior change.
- The **current** keyspace implementation, which threaded each created slot into
  that global runtime list.

### 5.2 Lean keyspace redesign

Keep the user-facing capability — heap-free, RPC-creatable named entries that
survive reboot — but make a keyspace **self-contained** instead of borrowing the
global runtime list.

Core idea: **a keyspace is its own compile-time-registered object that owns a
fixed slot array; its live slots are reached by iterating keyspaces, not a
global setting list.**

1. `ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE` emits a
   `STRUCT_SECTION_ITERABLE(zmk_custom_setting_keyspace, name)` plus a static
   `slots[max_entries]` array and one `ZMK_CUSTOM_SETTING_LARGE_POOL`. No `_next`
   field, no `zmk_custom_settings_register_keyspace()` runtime call — the linker
   section *is* the registry. (Drop that register function.)
2. **A slot is just a plain pooled BYTES setting** whose value is the whole
   entry, `[key\0][payload]`, as one opaque blob (§5.3). All existing
   read/write/save/pool code that takes a `struct zmk_custom_setting *` works on
   it unchanged, knowing nothing about the key. Create = claim a free slot +
   write the blob; delete = erase its record + release its pool region.
3. Only the **few** sites that must see user entries iterate keyspaces
   explicitly, via a small, contained helper (e.g. `FOR_EACH_KEYSPACE_SLOT`):
   - `find`-by-key (get/write of a created entry),
   - the RPC `ListSettings`/`GetSetting` handlers,
   - `save`/`discard`/`reset` scope application.
   These are also the only places that **decode** the blob into (key, payload)
   — the single piece of keyspace-specific logic, living at the presentation
   boundary, not in storage/pool/value. A deliberate, local coupling in ~3
   places, versus today's invasive global-list merge that forced *every*
   iteration through a virtual iterator.
4. **Persistence/reboot:** slots persist under a stable **ordinal** storage name
   (`"<subsystem>/<prefix>#<i>"`), not the user key. The
   `SETTINGS_STATIC_HANDLER` load callback matches the keyspace by prefix, binds
   slot `i` from record `#i`, and lets the generic value-load path fill its blob
   — the user key is recovered for free by decoding the blob, so there is **no
   separate key persistence and no key table**. The "don't nest
   `settings_load_subtree`" discipline is unchanged.
5. **The entire entry is one pooled value (§3.4).** No fixed
   `keys[max_entries][max_key_len]` or `value_bufs[max_entries][max_size+1]`
   tables: the blob is carved from the keyspace's pool exactly like any
   `DEFINE_POOLED` BYTES value. Only the fixed `slots[max_entries]` descriptor
   array remains. See §5.3.

**Net:** `CreateSetting`/`DeleteSetting` RPC and the web create/delete UI are
kept (the capability the owner wanted), but the machinery underneath shrinks to
a compile-time section + a 3-site slot iterator + reuse of the pool primitive,
and the general runtime-registration API disappears.

### 5.3 The whole entry is one opaque value

The key simplification: **treat an entire keyspace entry — its key *and* its
payload — as a single opaque BYTES value, `[key\0][payload]`.** The
storage, pool, and generic value-read/write/save code then handle a keyspace
slot as a bog-standard pooled BYTES setting with *zero* keyspace-specific
knowledge. The (key, payload) structure is a convention that only the create /
find / list / get boundary encodes and decodes.

| Per-entry data | Storage | Note |
|---|---|---|
| The whole entry as one opaque BYTES blob `[key\0][payload]` | the keyspace's pool (§3.4), carved on demand — just a `large_pool` member | pool/value code is 100% generic: one `large_data`/`large_size` per member, one-pointer compaction, nothing keyspace-aware |
| Slot descriptor + `in_use`/`dirty`/temp state | fixed `slots[max_entries]` array | **cannot** be pooled (compaction moves regions; `struct zmk_custom_setting *` pointers are held widely) |

Why this is simpler than a key/value split inside the region:

- **No `value_offset`, no two-pointer compaction.** The slot's `large_data`
  points at the blob start and `large_size` is the blob length — exactly the
  ordinary pooled-BYTES contract. Compaction moves the region and updates the
  one `large_data` like any other member. The pool never learns what a key is.
- **No separate key storage.** The key rides inside the value, in the pool.
  There is no `keys[][]` table and no key-persistence path; on reboot the key is
  recovered by decoding the loaded blob (§5.2 point 4).
- **Uniform wire format.** `GetSetting`/`ListSettings` decode the blob and put
  the key in `Setting.key` and the payload in `Setting.value`, so clients see a
  keyspace entry identically to a normal setting. `CreateSetting`/`WriteSetting`
  encode `(key, payload) → blob`. That encode/decode is the *only* new logic,
  and it is confined to the ~3 presentation/lookup sites.

Consequences:

- **One large-value path.** A payload ≥ carrier rides the same pool + streaming
  `GetSetting` (§4) + `WriteValueChunk` path as any pooled BYTES setting.
  `value_bufs`/`value_buf_stride` and the fixed key table are all deleted.
- **Flexible budget instead of worst case.** A pool smaller than
  `max_entries × max_size` lets a keyspace hold *many small* entries or *few
  large* ones from one budget — the over-commit that motivated `DEFINE_POOLED`
  for `runtime-macro`. An empty keyspace's RAM floor is `slots[]` only; every
  key+payload byte tracks real usage.
- **`find` decodes to compare.** Lookup walks the ≤`max_entries` live slots and
  compares the requested key against each blob's decoded key prefix — a short
  walk, no linear scan of raw pool bytes.
- **Region lifecycle = entry lifecycle.** Because the key is always present, a
  slot's region exists from **create → delete** (it does not free on an empty
  payload the way a plain pooled setting does). 1:1 with the entry — simpler to
  reason about — but an empty-payload entry still costs `keylen + 1` pool bytes.
- **`create` can hit `-ENOSPC`.** If the pool cannot fit the new blob, create
  fails and leaves prior entries untouched (pooled-write semantics). A persisted
  entry that no longer fits on load is skipped with a `LOG_WRN`, never fails
  boot.
- **Typed payloads still validate.** The keyspace keeps its declared
  `value_type`/constraints; create decodes the blob's payload as that type and
  runs the normal `zmk_custom_setting_validate` before committing.
- **Caveat.** Compaction is O(n²) in a pool's live entries (small).

In short, a keyspace becomes ≈ *a fixed array of anonymous pooled BYTES
settings addressed by ordinal, plus a decode convention for (key, payload)* —
almost no mechanism beyond what §3 and §4 already build.

#### Does `max_entries` still make sense with a pool? Yes — it bounds a different resource.

The pool removes the per-entry *byte* bound (worst-case `keys[][]` +
`value_bufs`), **not** the *entry-count* bound. Each live entry still needs one
fixed-size, **non-poolable** thing: its slot descriptor in `slots[max_entries]`
(a full `struct zmk_custom_setting`, ~100+ bytes since it embeds a
`memory_value` carrier). `max_entries` bounds those. It cannot be dropped
without either:

- **heap allocation** for descriptors — violates the heap-free goal; or
- **storing descriptors in the pool** — impossible, because the pool compacts
  (memmoves regions) and the codebase passes `const struct zmk_custom_setting *`
  around widely (read handlers, RPC, events). A moving descriptor would
  invalidate those pointers, requiring a far stronger "never cache a descriptor
  pointer across a lock" invariant than the existing "never cache `large_data`".
  (The blob *bytes* — key and payload alike — can move freely; nothing caches a
  pointer into a pool region across a lock. Only the descriptor is pinned.)

So after this change the two limits are **orthogonal and both meaningful**:

- `max_entries` — how many entries can exist at once (bounds the descriptor
  array).
- pool size — total blob (key+payload) byte budget shared across those entries.

e.g. `max_entries = 32` with a pool sized for 8 max-size values = "up to 32
small entries, or a handful of large ones." Reframe the macro/README docs for
`max_entries` accordingly ("max concurrent entries", independent of key/value
size), rather than removing it.

*Optional future optimization (out of scope):* the per-entry fixed cost could
be shrunk with the array feature's view-pool trick — keep a compact per-entry
record (key + pool region handle + flags) and synthesize a transient
`struct zmk_custom_setting` from a small shared view pool on access. That lowers
the RAM price of a generous `max_entries` but does **not** remove the parameter
(the compact-record array is still sized by it) and adds a view mechanism, so
it is deferred.

> Decision to confirm: keeping keyspaces at all is optional. If they turn out
> not to be worth even the lean form, deleting `CreateSetting`/`DeleteSetting`
> and the whole keyspace section is strictly less code. The owner asked to keep
> a *simplified* keyspace, so this proposal keeps it.

---

## 6. Change 4 — split `struct zmk_custom_setting` into flash meta + RAM state

### 6.1 Problem: ~160 RAM bytes per setting, mostly wasted

Every descriptor is one monolithic `struct zmk_custom_setting` placed in RAM
(`ITERABLE_SECTION_RAM(zmk_custom_setting, 4)` in
`include/linker/zmk-custom-settings.ld`, included via
`zephyr_linker_sources(DATA_SECTIONS …)`). On ARM32 it is ≈160 bytes, split
roughly:

| Portion | ≈bytes | Actually needs RAM? |
|---|---|---|
| `memory_value` (union sized by `VALUE_MAX_SIZE`=64, + type + size) | 76 | **No** for scalars — a bool/int32/behavior needs 1–12 bytes; BYTES/STRING move behind a pointer anyway (§3) |
| Immutable metadata: ids, key, `value_type`, confidentiality, permissions, constraints ptr+count, `default_value`, converters, `value_max_size`, `large_pool`, array wiring | ~60 | **No** — written once by the registration macro, never mutated → belongs in flash/rodata |
| Genuinely mutable state: `initialized`/`has_persistent_value`/`dirty`/`temporary_active`, `temp_slot`, `large_data`/`large_size` | ~20 | Yes |

So a bool setting pays ~160 RAM bytes to track ~2 bytes of state. Two
orthogonal fixes, both enabled by the accepted compat break (after §5 removes
runtime registration, the macros are the *only* construction path, so the
layout is free to change):

### 6.2 Const/mutable split (sub-struct reference)

Make the section-iterated object **const and flash-resident**, holding a
pointer to a small RAM state block:

```c
/* Flash (rodata). This remains the public handle type: every API keeps
 * taking const struct zmk_custom_setting *. */
struct zmk_custom_setting {
    const char *custom_subsystem_id;
    const char *key;
    uint8_t value_type;              /* enums packed to uint8_t */
    uint8_t confidentiality;
    uint8_t read_permission : 4, write_permission : 4;
    uint8_t constraints_count;       /* was size_t */
    const struct zmk_custom_setting_constraint *constraints;
    const struct zmk_custom_setting_value *default_value;
    zmk_custom_setting_rpc_bytes_converter_t rpc_serializer, rpc_deserializer;
    union {                          /* kind-specific, mutually exclusive */
        struct {                     /* BYTES/STRING */
            uint32_t max_size;
            struct zmk_custom_setting_large_pool *pool;
        } blob;
        struct zmk_custom_setting_array_state *array;  /* arrays (replaces
                                        array_key/array_state; array_index
                                        lives only on index views) */
    };
    struct zmk_custom_setting_state *state;   /* → RAM, macro-emitted */
};

/* RAM. One per setting, emitted by the same macro. */
struct zmk_custom_setting_state {
    uint8_t flags;      /* initialized | has_persistent | dirty | temp_active */
    int8_t temp_slot;
    union {             /* the effective value, right-sized per kind */
        int32_t int32_value;
        bool bool_value;
        struct zmk_custom_setting_behavior_value behavior;   /* 12B */
        struct { uint8_t *data; uint16_t size; } blob;       /* → §6.3 */
    };
};                       /* ≈ 16 bytes */
```

The linker include moves from `ITERABLE_SECTION_RAM` to
`ITERABLE_SECTION_ROM`, putting descriptors in rodata. Access to mutable
fields becomes `setting->state->…` — a mechanical, internal-only churn
(`grep`-able; the public API surface is unchanged since callers only hold
`const struct zmk_custom_setting *` and use accessor functions already).

### 6.3 Kill the embedded 64-byte carrier (union by kind)

The second fix follows from §3/§4: once BYTES/STRING values live behind a
pointer (a pool region, or a right-sized macro-emitted static buffer for
`DEFINE`-registered small values), **no descriptor needs an embedded
worst-case `memory_value` union at all**:

- scalars (`INT32`/`BOOL`/`BEHAVIOR`): value inline in `state` (≤12B union);
- `BYTES`/`STRING`: `state.blob = {data, size}` pointing at the pool region or
  the setting's own exact-size buffer (`uint8_t name_store[max_size + 1]`),
  so a 5-byte-max setting costs 6 bytes, not 64.

`struct zmk_custom_setting_value` (the 64B carrier) survives **only as the
stack-allocated API exchange type** for `zmk_custom_setting_read/write` — it is
no longer embedded anywhere per-setting. The view-based APIs
(`read_into`/`write_bytes`/`with_value`) bypass it entirely, as today. This is
the "full P1 union removal" that PR #24 deliberately deferred for
backward-compat of hand-built descriptors — a constraint that §5 (deleting
runtime registration) removes.

### 6.4 Expected savings & knock-on effects

Per setting: ≈160 RAM B → ≈16 RAM B state + exact value bytes + ≈40 flash B
meta. For the 9-setting sample config that is roughly 1.3 KB RAM back on top
of P1's earlier 26 KB reduction; flash cost is negligible.

Knock-on wins:

- **Keyspace slots (§5.3) shrink the same way.** A slot descriptor no longer
  embeds a 76B carrier, so the *non-poolable* fixed cost per slot —
  the very thing `max_entries` exists to bound — drops from ~160B to a few
  tens of bytes (slot descriptors stay in RAM since their identity fields are
  runtime-bound, but they share the same shrunken layout). A generous
  `max_entries` becomes cheap, largely mooting the deferred view-pool idea.
- **Array index views shrink identically** (they are RAM instances of the same
  struct handed out by `array_view_pool`).
- `VALUE_MAX_SIZE`'s role narrows to "single-frame RPC value bound + API
  carrier size" — it no longer sizes any per-setting storage.

### 6.5 Costs / risks

- Internal churn: every `setting->dirty`-style access becomes
  `setting->state->…`. Mechanical but touches most of `custom_settings.c`.
- `STRUCT_SECTION_ITERABLE` of a `const` object in ROM: standard Zephyr
  practice, but the module's `.ld` include and macro qualifiers must move
  together (`DATA_SECTIONS` → `ROM_SECTIONS`, descriptors declared `const`).
- Ordering: land **after** Changes 1–3 (they delete the fields/paths —
  `large_data` dual-role, `_runtime_next`, hand-built-descriptor compat — that
  would otherwise complicate the split).

## 7. Resulting public interface

**Removed**

- `zmk_custom_settings_register()`, `zmk_custom_settings_unregister()`,
  `zmk_custom_settings_foreach_first/next`, the `_runtime_next` field.
- `zmk_custom_settings_register_keyspace()` (replaced by the linker section).
- Proto: `ReadValueChunkRequest`; `Request.read_value_chunk`. `ValueChunkResponse`
  keeps only its write-ack role (or is removed if the write path doesn't need a
  data echo — TBD in §10).
- `CONFIG_ZMK_CUSTOM_SETTINGS_*` for the read chunk path (fold into write-only).

**Changed**

- `ZMK_CUSTOM_SETTING_DEFINE_SIZED`: either sugar over `DEFINE_POOLED`
  (source-compatible, kept for convenience) **or** deleted. Recommendation: keep
  it as the 2-line sugar so "one standalone large setting" stays ergonomic while
  the core carries only the pool path.
- `ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE`: same surface, now a compile-time section
  entry backed by a pool for large values; `register_keyspace()` no longer
  needed.
- `ZMK_CUSTOM_SETTING_FOREACH`: plain section walk (implementation detail;
  same usage).

**Kept unchanged**

- `DEFINE`, `DEFINE_POOLED`, `LARGE_POOL_DEFINE`, `ARRAY_DEFINE` and their
  `_WITH_*` variants; all read/write/view/typed APIs; save/discard/reset;
  records; `WriteValueChunk`; TEMPORARY mode; converters.

---

## 8. Migration for the only external consumer

`zmk-feature-runtime-macro` uses `DEFINE`, `LARGE_POOL_DEFINE`, and
`DEFINE_POOLED` — **none of which change semantically**. If `DEFINE_SIZED` is
kept as sugar, that repo needs **zero changes**. If `DEFINE_SIZED` is removed,
that repo is still unaffected (it never used it). No storage-schema change is
required for pooled values, so persisted macros survive the upgrade.

The read-chunk removal is a client/web concern; firmware consumers are
unaffected. Any web client pinned to `chunkedValue.ts`'s read path must switch
to plain `GetSetting` (which now returns the full value).

## 9. Compatibility notes (breaks are intentional)

- **Wire protocol:** removing `read_value_chunk` (tag 9 in `Request`) and
  shrinking `Response` is a breaking proto change. Web UI and firmware ship
  together in this repo, so they update in lockstep; external RPC clients must
  regenerate.
- **Storage schema:** unchanged for scalar/pooled settings (SIZED→POOLED does
  not alter stored bytes). **Keyspace entries change**: they now persist under
  an ordinal name (`"<subsystem>/<prefix>#<i>"`) with the value being the
  `[key\0][payload]` blob, instead of under the user key with a bare payload.
  Backward-compat may break (accepted); if preserving old keyspace records
  matters, add a one-shot migration that rewrites them on first boot.
- **RAM:** Changes 1–3 are roughly neutral (SIZED→one-member-pool adds a small
  pool descriptor per ex-SIZED setting; removing `chunk_read_buffer`,
  `keys[][]`, and `value_bufs` gives more back). Change 4 is the big win:
  ≈160 → ≈16 RAM bytes per setting plus exact value storage (§6.4). Measure on
  the sample board config as #24 did.

## 10. Phased implementation plan

Follow the repo rule: proto → firmware handler → web, tests at each milestone.

1. **Change 1 (pools).** Reimplement/remove `DEFINE_SIZED`; delete the
   fixed-buffer core path; convert the pool to own its members via an intrusive
   list (§3.4) so it no longer depends on `ZMK_CUSTOM_SETTING_FOREACH` (this
   also unblocks §5). Update tests (`custom_settings_test.c`, snapshots) and
   `test.py` build coverage.
2. **Change 2 (read chunk).** Proto: drop `ReadValueChunkRequest`. Firmware:
   value-encode callback on `Setting.value`; delete read handler + staging;
   confirm large `GetSetting` streams on hardware (reuse #24's `large_bytes`
   setup). Web: delete read half of `chunkedValue.ts`, use `GetSetting`.
   Decide whether `ValueChunkResponse` survives for the write-ack.
3. **Change 3 (runtime reg + lean keyspace).** Remove register APIs, virtual
   iterator, `_runtime_next`; revert `FOREACH` to a plain section walk. Rebuild
   keyspace as a compile-time section + 3-site slot iterator where **each entry
   is one opaque `[key\0][payload]` pooled BYTES value** (§5.3, relying on
   §3.4's pool member list); delete the fixed `keys[][]` table and
   `value_bufs`/`value_buf_stride`; switch storage naming to ordinals; add the
   blob encode/decode at create/find/list/get. Keep the settings-load bind path
   and `CreateSetting`/`DeleteSetting`. Update web create/delete (should be
   minimal). Re-run the split relay + hardware checks the keyspace feature
   relied on.
4. **Change 4 (descriptor split).** Deliberately last — Changes 1–3 delete the
   fields and compat constraints that would complicate it. Pack enums, split
   const meta (→ rodata, `ITERABLE_SECTION_ROM` + `ROM_SECTIONS` linker move)
   from RAM state, remove the embedded `memory_value` carrier (§6.3). No proto
   or web change; measure RAM on the sample board config before/after (as P1
   did).

Each phase is independently landable and independently reduces surface area;
they can be separate PRs in the order above.

## 11. Open questions

1. `DEFINE_SIZED`: keep as sugar (source-compatible) or delete outright? (Rec:
   keep as sugar.)
2. Does the write path still need `ValueChunkResponse` to echo progress, or is a
   plain `StatusResponse` ack enough? If the latter, delete `ValueChunkResponse`
   too.
3. Keyspace values are pool-backed (§5.3). Remaining sub-question: one pool
   *per keyspace* (isolated budgets, recommended default — a full keyspace
   can't starve another) vs one pool *shared across all keyspaces* (lower total
   RAM). The macro can default to a private per-keyspace pool while allowing an
   explicit shared-pool variant for advanced callers.
4. Should the value-encode callback also finally apply the RPC serializer
   converter on the large path (currently skipped), now that there is a single
   encode path? (Out of scope unless cheap.)
5. Change 4: for small `DEFINE`-registered BYTES/STRING values, right-sized
   per-setting static buffer (recommended: exact-size, no pool motion) vs
   routing even small values through a pool? Either way the descriptor stops
   embedding the 64-byte carrier.

## 12. Implementation notes (Phase 1 / Change 1, as landed)

Phase 1 (this module's `codex/simplify-p1-pools` branch) implemented §3 in
full, plus the pool-ownership prerequisite of §3.4, plus a narrow slice of
§5.3 (only the value storage swap, not the opaque-blob/ordinal-persistence
keyspace redesign, which stays Phase 3 as planned). Noted deviations from the
sketches above:

- **Pool member link field name.** §3.4's sketch calls the intrusive-list
  field `pool_next`; the shipped field is `_pool_next` (leading underscore),
  matching this file's existing `_runtime_next` convention for
  internal/do-not-touch struct fields.
- **`DEFINE_SIZED`'s private pool is sized `max_size + 1`, not `max_size`**
  (§3.2's sketch pool size). A `STRING` region's trailing NUL is stored
  *inside* the pool region (`large_store_set_raw`), so a full-length `STRING`
  needs `max_size + 1` bytes to still fit - the same `+1` the old dedicated
  buffer already reserved. Sizing the private pool at exactly `max_size`
  would have been a capacity regression for `STRING` settings.
- **Extra macro layer.** `DEFINE_SIZED`'s full signature (RPC converters +
  variadic constraints) required a new innermost
  `ZMK_CUSTOM_SETTING_DEFINE_POOLED_WITH_RPC_CONVERTERS_AND_CONSTRAINTS` macro
  (not spelled out in §3.3) so both `DEFINE_POOLED` and `DEFINE_SIZED` funnel
  through one place that actually sets `.large_pool`. The plain-`DEFINE`
  innermost macro (`..._WITH_RPC_CONVERTERS_AND_CONSTRAINTS`) is now the one
  that emits no large-value store of any kind, so a normal small setting pays
  nothing extra.
- **§11 open question 3 resolved:** each keyspace gets its own **private**
  pool (isolated budget), matching the recommended default; the "shared
  across all keyspaces" variant was not built (no caller has asked for it).
- **Keyspace storage swap is intentionally narrower than §5.3.** Task 3 kept
  `keys[max_entries][max_key_len]` and `slots[max_entries]` exactly as they
  were - only the large-value backing store moved from a fixed
  `value_bufs[max_entries][max_size + 1]` table to a per-keyspace
  `ZMK_CUSTOM_SETTING_LARGE_POOL` (same worst-case budget, carved on demand
  per slot via the now-generic pooled-setting path). The `[key\0][payload]`
  opaque-blob encoding, ordinal persistence naming, and removal of the
  runtime-registration facility described in §5 are unchanged/deferred to
  Phase 3.
