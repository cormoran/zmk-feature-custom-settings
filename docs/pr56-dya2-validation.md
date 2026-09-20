# PR #56: latest DYA2 validation

Status: software regressions reproduced and fixed; all pre-commit checks and
standard tests passed. Hardware validation in progress (2026-09-20 UTC).

## Revisions and scope

- DYA2 `main`: `eca79a3a9adfb0b9015508db1fa4572f6680152f`.
- Custom settings baseline: `c6a7fef3a3be3d3ace5de9a4b0628c6418cd1f3f`.
- PR #56: `85e3ca84385842598e1a7a2d8633ebe7b9a3b014`.
- DYA2 manifest's moving module branches were fetched on the validation date.
- Full resolved checkout inventory: [pr56-dya2-sources.tsv](pr56-dya2-sources.tsv).
  All checked-out dependencies were clean. Unchecked-out BabbleSim projects
  are not used by these XIAO/native_sim builds. For PR/fixed builds only the
  custom-settings entry is overridden with the specified review checkout.
- Existing working copies, including a modified DYA2 config, are not used as
  the release source. Builds use a separate checkout of upstream DYA2.
- Single XIAO attached to J-Link: device work delegated to a Luna subagent.
  Two-device wired/BLE relay and physical trackball/key scanning cannot be
  established by single-board RPC checks alone.

## 方針の評価

RAM 削減の方針は妥当。ただし PR 原版には以下の再現可能な回帰があるため、
そのままの採用は推奨しない。本 follow-up で修正と回帰テストを追加する。
consumer の compact/fixed-size 移行と各モジュールの shared response 化は、
保存済み値の最大サイズも調べた別の変更として進める。

The callback conversion removes recursively embedded constraint arrays from
protobuf response and notification objects without changing field numbers.
Compact arrays and fixed-size scalar storage require consumer migration;
the latest DYA2 dependencies do not use the new registration macros.

The latest DYA2 ZMK RPC loop calls `handle_request()` followed immediately by
`send_response()` on the same thread. Notifications synchronously encode via
the event mapper and notification listener. This supports the helper's
storage lifetime assumptions for these callers; a generation counter is not
a mutex and does not make arbitrary concurrent allocators safe.

### 1. Compact array capacity rejection mutates data

PR #56 validates the global carrier size before shifting elements, but checks
the smaller compact capacity only when storing the inserted value. An
oversized insert can therefore modify elements even though it returns
`-EMSGSIZE`. Push similarly grows the array before detecting that error.
With default bytes `[11], [22]`, capacity 2, an insert of `[1,2,3]` at index
zero returned `-90` and changed index 1 to `[11]`. Native regression output:
`FAIL: compact rejected insert changed existing elements (ret=-90)`.

Fix: validate encoded capacity through the shared public validation path,
before mutation, including temporary writes. Numeric/behavior serialization
uses the settings mutex because the behavior encoder has shared scratch.
Also reject zero capacity and capacities that cannot fit the length byte.

### 2. Fixed-size values silently truncate oversized writes

The existing blob store applies `MIN(value_length, setting_capacity)`.
After opting into capacity 4, writing STRING `abcde` therefore succeeded and
stored `abcd`. The ordinary carrier bound (64) did not protect the smaller
backing store. Added test fails on the PR version and checks exact-capacity
write/persist/reset, rejection beyond capacity, and read termination.

Fix: validate BYTES/STRING against the setting's capacity before storing.
Compile-time defaults must still be chosen to fit by the registering module.

### 3. Split relay drops constraint metadata

After `SettingMeta.constraints` becomes `FT_CALLBACK`, the central decode
path needs a decoder for that repeated field. Without one, nanopb skips it;
republishing a peripheral's ListSettings notification silently loses its
constraints. An INT32 notification with a range constraint shrank from 39
to 27 bytes on decode/re-encode. Values-only regression tests missed this.

Fix: retain encoded constraint fields in a relay-payload-bounded scratch
buffer, then stream them back on encode. This retains the RAM benefit and
does not depend on finding a matching local registration for a remote setting.
The regression now compares the complete notification wire before/after
re-encoding for both INT32 and STRING, including metadata.

## Build measurements

Identical target: `right_trackball`, board `xiao_ble//zmk`, shield
`dya2_right`, snippets `studio-rpc-usb-uart` and `right-trackball`.

| Custom settings | FLASH bytes | RAM bytes |
| --- | ---: | ---: |
| baseline main | 422836 | 161576 |
| PR #56 original | 422152 | 132712 |
| PR #56 + this correction | 422440 | 132968 |

PR #56 saves 28,864 bytes of RAM (28.19 KiB, 17.86% of baseline allocation)
and 684 bytes of FLASH. It leaves 129,432 bytes outside the linker's RAM
allocation on a 256 KiB nRF52840. The original PR's left target also builds:
252,468 bytes FLASH / 62,712 bytes RAM.

The reduction is not evidence that downstream modules use compact arrays.
ELF symbols show the principal changes:

| Object | Baseline bytes | PR #56 bytes |
| --- | ---: | ---: |
| custom-settings response / shared response buffer | 7012 | 264 |
| custom-settings notification buffer | 7020 | 272 |
| relay notification decode buffer | 7056 | 44 |
| peripheral-only relay response buffer on central | 7012 | eliminated |

Additional queue/buffer removal and alignment account for the remaining
total. Other modules still allocate their own response buffers (for example
runtime combo: 2760 bytes). The shared helper does not consolidate these
unless they explicitly migrate.

These are linker allocations, not measured runtime stack high-water marks.
The corrected right target still saves 28,608 bytes RAM (27.94 KiB) and 396
bytes FLASH against baseline. Corrected left: 252,596 bytes FLASH / 62,712
bytes RAM. Corrected unlocked right: 422,296 bytes FLASH / 132,968 bytes RAM.
The release config keeps Studio thread stack 6000 and low-priority/system
workqueue stacks 4096 bytes; INIT_STACKS and THREAD_ANALYZER are disabled.
Therefore the RAM result does not justify reducing those stacks.

## Software validation results

`pre-commit run` passed, including the repository's `python3 -m unittest -v`:

- Native `test`, `studio`, and `split_peripheral` snapshots pass.
- Six XIAO configurations pass config/devicetree/artifact checks: feature
  disabled, minimal, with RPC, without RPC, split central, split peripheral.
- Fixed DYA2 right, unlocked right, and left produce HEX/ELF/UF2 artifacts.
- Clang-format, Ruff, whitespace, conflict, and file-size hooks pass.
- No Web code or protobuf schema was changed; Web hooks correctly skip.

GitHub CI for code commit `9ace6052fd7c979f0e0173baf8b4e190c93ab78b` also
passed: [firmware Build and Renode wired-split relay](https://github.com/cormoran/zmk-feature-custom-settings/actions/runs/35522278357),
and [Web tests/build](https://github.com/cormoran/zmk-feature-custom-settings/actions/runs/35522278331).
The Renode result is simulated two-device integration, not physical split
validation on the attached single XIAO.

The new tests first failed against the PR implementation, then passed with
the fixes. Stricter scalar validation also requires the temporary keyspace
payload descriptor to carry its `max_size`; the existing large-keyspace
tests caught that integration requirement, and it is included in the fix.

## Reproduction

Use the workspace Nix devShell and the DYA2 manifest. Keep baseline, PR
original, and corrected module checkouts separate; all other dependencies
must match. A module checkout passed with `-m` overrides the manifest's
`zmk-feature-custom-settings` module by its declared module name.

```sh
west zmk-build . --artifact-filter '^right_trackball$' -d build/baseline -q
west zmk-build . --artifact-filter '^(right_trackball|left)$' \
  -d build/pr56-original -m /path/to/pr56-original -q
west zmk-build . --artifact-filter '^(right_trackball|left|right_trackball_studio_unlocked)$' \
  -d build/fixed -m /path/to/pr56-review -q
west zmk-test /path/to/pr56-review/tests -m /path/to/pr56-review -d build/native
```

The repository's standard `python3 -m unittest -v` additionally verifies six
XIAO module configurations and the native core/Studio/peripheral suites.
The `_reset` firmware targets are deliberately excluded from hardware tests.
The unlocked image is an explicit test configuration, not the release image.

HEX SHA256 identities (fixed sources were committed as `9ace605` after
building; later report-only commits do not alter these artifacts):

```text
baseline right: a31ebf03271c0bf6130754b9eb8bc677ea0e423991d711e8f66fc178c7c92950
PR #56 right:   dfab74f7fd1654213d167780cbacdfd665a86fd60b718669851d66b12f06fd49
fixed right:    a9056cf19db33bf12e20ae85899143ae87203b9c236068aaa5ff44fa9efd1767
fixed unlocked: 8dcd671c85582c4f7a18c72ae496b6bbf8f88965f57db023ad5f8609ae109893
fixed left:     e2126d8689ea24f0c1599d2a7fef76763315ae29cbd2b1a0aeeb50eb71d507bc
```

## Hardware progress

Initial discovery identified a J-Link OB-nRF5340-NordicSemi probe, serial
`1057792823`, VTref 3.300 V, and SW-DP ID `0x2BA01477`. Attach to
`NRF52840_XXAA` at SWD 4000 kHz failed with `Failed to power up DAP`.
No XIAO USB/CDC port was enumerated. No flash or settings write was performed
on that first target; slower SWD and reset-line checks also failed.

After the user exchanged the XIAO and J-Link, probe `1050398082` successfully
attached to nRF52840, board serial `0C5B206D3B120A9F`. The hardware agent
backed up 1 MiB flash and 4 KiB UICR locally, then flashed and verified the
baseline. Backups are private local artifacts, not committed to this repo.

The generated DTS names `board_cdc_acm_uart` and
`snippet_studio_rpc_usb_uart`, but the board console node is disabled;
Studio's chosen node is the latter. Thus two node names do not imply two
active CDC ports. Initial requests timed out. Transport/running-state checks
are in progress; no runtime pass is claimed yet.

Luna completed discovery/backup/baseline flashing. Terra took over the USB
RPC diagnosis after the interrupted session, with an explicit lock handoff.
