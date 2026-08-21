# Host-test inventory

Every `tools/test_*.c` in the repository, classified. Established 2026-08-21.

Authoritative runner: `pwsh -File tools/run_host_tests.ps1`. The manifest lives in that script;
this document explains the classification and the reasoning behind the exclusions.

## Why this audit happened

Cleaning `build/` revealed that only 23 of 79 host-test sources had a build recipe. The other 56
existed as executables accumulated in `build/host-tests` from ad-hoc `gcc` invocations, and
`AGENTS.md` instructed readers to run whatever binaries were present. Totals produced that way
described a directory, not the source tree.

**Negative knowledge:** a host-test total is only evidence about current source if every counted
test was compiled during the run that reported it. Previous "76/76", "78/78", and "79/79" figures
are withdrawn. Do not restore them.

## Categories

| Code | Meaning |
|---|---|
| **A** | Current reproducible host test — had a recipe before this audit |
| **B** | Legitimate host test that was missing a recipe — reconstructed and now in the manifest |
| **C** | Not a host test — materially depends on Pico SDK, BTstack, or an external library |
| **D** | Obsolete/superseded — references removed code or an outdated production signature |
| **E** | Experiment/diagnostic harness with its own builder |

## Totals

| Category | Count |
|---|---|
| A — already reproducible | 23 |
| B — reconstructed in this audit | 47 |
| **Active host suite (A + B)** | **70** |
| C — not host tests | 5 |
| D — obsolete | 3 |
| E — lab harness | 1 |
| **Total `tools/test_*.c`** | **79** |

The category-A baseline is the working tree at the start of the audit: 22 recipes committed in
`tools/run_mgmt_tests.ps1`, plus `test_ns2_lifecycle_model`, which arrived with the in-flight
Bluetooth reliability work and shipped with its own recipe.

70 declared active host-test targets rebuild from current source and pass. The 9 remaining sources
are declared in `$notHostTests` with a reason, and the runner refuses to start if any
`tools/test_*.c` is in neither table.

This is not the same claim as "all tests pass". Five of the nine excluded sources
(**category C**) are genuinely uncovered on the host, and three (**category D**) represent coverage
that was lost when the code they exercised changed or was removed — see those sections below.

## C — not host tests

Host-linking these would mean mocking the Pico SDK or BTstack. The pure logic they sit behind is
already covered by other tests in the suite, so a mock environment would buy coverage the project
already has, at the cost of a second fake platform to maintain. **No production code was refactored
to make any of them compile** — test infrastructure must not drive architecture.

| Test | Blocker |
|---|---|
| `test_bthid_late_identity` | needs `bthid.c`, which needs the Pico SDK |
| `test_ns2_motion_probe` | `ns2_motion_probe.c` includes `pico/time.h` |
| `test_ns2_native_motion` | `ds5_motion_pair_capture.c` includes `pico/critical_section.h` |
| `test_ns2_wake_policy` | `btstack_host.h` includes BTstack's `bluetooth.h` |
| `test_ds5_audio_tone` | needs the vendored Opus library; belongs to the audio lab tooling |

Note that `test_ns2_active_input_lifecycle` *is* in the suite and does link `bthid.c` — it uses
`tools/host_stubs` and `--gc-sections` to avoid the SDK-dependent paths. That trick did not extend
to `test_bthid_late_identity`, which reaches code the linker cannot discard.

## D — obsolete

Left on disk as history; not resurrected, not deleted.

| Test | Reason |
|---|---|
| `test_ns2_ds5_motion40` | `ns2_motion40_catchup` was added in `25ccf86`/`19322ca` and later removed when the length-`0x28` catch-up path was deferred to research-only. The header no longer exists anywhere in the tree. |
| `test_ns2_motion_pdu40` | same removed module |
| `test_ds5_audio_packet` | asserts an older `ds5_audio_build_stream_report()` signature — argument 7 changed type, so the test cannot compile against current production. Superseded by `test_ds5_audio_control` and `test_switch2_pro2_audio_transport`. |

## E — lab harness

| Test | Reason |
|---|---|
| `test_gcusb_core` | belongs to the standalone GameCube USB lab tool; built by `tools/gcusb/build.ps1`. Cited by `docs/experiments/gcusb-rumble-lab-2026-07-14.md`. |

## Recipes worth knowing about

Three needed something non-obvious, all documented inline in the runner:

- `test_ns2_protocol_trace` — **`-DNS2_UART_DIAG`**. The trace ring is compiled out without it, so
  the module silently accepts and discards every record. Restoring this test without the define
  produced a failing assertion that looked like a production bug and was not one.
- `test_usb_mode_cycle` — **`-DNS2_PRO`**. `usb.h` publishes `usb_personality_t` only for the
  Switch 2 firmware.
- `test_ds5_audio_control` — **`-Wno-unused-function`**. Built in the non-audio configuration; the
  bridge's diagnostic helpers are referenced only by the live-audio paths.

## Method, and why the recipes are hand-checked

Candidate source sets were discovered by compiling each test and resolving exactly what the linker
asked for, then reviewed by hand. That review mattered: a greedy "add sources until it links" pass
pulled `src/switch_pro/switch_pro.c` — and with it the Pico SDK — into `test_ns2_motion_seam` and
`test_ns2_motion_quality`, which in fact need only `ns2_motion_seam.c` (plus `ns2_motion_pdu.c` and
`ns2_ds5_motion.c` for the latter). Both would have been misclassified as platform-dependent.

Each manifest entry is the smallest intentional source set, not the first set that linked.

## Coverage relevant to the 2026-08 Bluetooth/management/input work

Of the 56 tests that previously had no recipe, **12** touch subsystems changed by that pass. All 12
are now in the active manifest and pass every run:

| # | Test | Subsystem |
|---|---|---|
| 1 | `test_ns2_pairing_crypto` | pairing / trust |
| 2 | `test_ns2_wake_protocol` | Bluetooth wake lifecycle |
| 3 | `test_bootsel_gesture` | pairing-window gesture |
| 4 | `test_bootsel_action` | pairing-window gesture |
| 5 | `test_controller_battery` | controller identity/battery |
| 6 | `test_ns2_diag_input` | input diagnostics |
| 7 | `test_ns2_protocol_trace` | diagnostics ring |
| 8 | `test_usb_mode_cycle` | USB/BT ownership boundary |
| 9 | `test_ns2_player_led` | console-slot output |
| 10 | `test_ns2_locked_mapping` | console-slot routing |
| 11 | `test_hid_out_normalize` | console-slot output |
| 12 | `test_bthid_mouse_report` | input source registry |

The count and this table are verified against `tools/run_host_tests.ps1` and against the recipe set
committed at the time the gap was found; each entry was absent from that set and is present in the
manifest now.

The core policy objects for that pass (`ns2_input_arbiter`, `ns2_bt_lifecycle`, `ns2_bt_health`,
`mgmt_access`, `config_wireless_bridge`, and the seeded lifecycle model) were already category A and
were never affected by the gap.
