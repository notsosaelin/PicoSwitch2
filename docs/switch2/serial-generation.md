# Serial Numbers — Structure, Meaning & Generation Design

> What the "serial number" actually is on a Switch 2 controller (there are **two different**
> surfaces, routinely confused), the byte-level structure decoded from **six genuine units**, what
> PicoSwitch2 emits today, and a design to generate **unique, format-valid, per-Pico** serials from
> the board's own hardware ID instead of the shared placeholder.
>
> **Documentation only — no code changed.** Companion to the per-personality identity blocks in
> `src/switch_pro2/switch_pro2.c`, `src/switch_gc/switch_gc.c`, `src/ns2_joycon2_identity.c`, and the
> unused `platform_get_serial` / `platform_get_unique_id` seam in
> `src/bt_hid/platform/platform.h`.
>
> Evidence: genuine SPI flash dumps (`dumps/SWITCH2_JOYCON_L_1.bin`, `_R_1.bin`, **decoded here**),
> live USB capture (`usbpcaptures/genuine_procon_2.pcapng`, serial `HEW70001504982`), ndeadly's
> `nso-gc-refs/switch2_controller_research/memory_layout.md`, and this repo's own
> `docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md`. Confidence: **Confirmed** (byte-verified
> here) · **Strong Evidence** · **Hypothesis** · **Unknown**.

## 0. Summary up front

- **There are two unrelated "serials."** Don't conflate them:
  1. The **USB `iSerialNumber` string descriptor** — genuine controllers report the literal string
     **`"00"`** (Confirmed for all four personalities). This is *not* a per-unit serial and **must
     stay `"00"`** for indistinguishability. Making it unique would make us *less* like genuine
     hardware.
  2. The **SPI factory identity serial** at flash offset **`0x13002`** — a **14-character
     per-unit** serial (`14 ASCII + 2 NUL` in a 16-byte slot). *This* is the real serial, and the
     one worth generating.
- **Structure decoded (Confirmed from 6 units):** `HB`/`HC`/`HE`/`HH` = model code (Joy-Con 2 L /
  Joy-Con 2 R / Pro Controller 2 / NSO GameCube). Char 1 is always `H`. The **trailing digits are a
  per-unit production sequence** (proven by the two GC and two Pro2 units, which share a prefix and
  differ only in the tail).
- **What we emit today is the problem:** every PicoSwitch2 unit ships the **same hardcoded serial**,
  and for Pro2 that hardcoded value (`HEJ71001121247`) is **a real third party's controller
  serial** copied from a reference doc — not unique, and arguably not ours to reuse.
- **Generation is feasible and clean.** The Pico's silicon has a unique 64-bit board ID
  (`pico_get_unique_board_id()`), exposed via the **already-declared-but-unimplemented**
  `platform_get_unique_id()`. Deriving the 14-char serial from it gives **stable, unique,
  format-valid** serials with the correct model prefix. Design in §7.
- **"Valid" ≠ console-checked.** The console pairs and streams fine with today's placeholder
  serials, so it does **not** validate serial content (Strong Evidence — absence of failure across
  hardware-validated Pro2/GC). "Valid" here means *format-plausible + host-cache-unique*, not
  cryptographically accepted. The practical payoff is uniqueness/indistinguishability, not
  unblocking the console.

## 1. Two serial surfaces — do not confuse them

| Surface | Where | Genuine value | Per-unit? | PicoSwitch2 action |
|---|---|---|---|---|
| **USB `iSerialNumber` string** | USB string descriptor index 3 | literal `"00"` (all 4 types) | **No** — shared across all real units | **Leave `"00"`.** Changing it reduces indistinguishability. |
| **SPI factory serial** | flash `0x13002`, 16-byte slot | 14-char string (see §3) | **Yes** — unique per physical unit | **Generate** (§7). Currently a shared placeholder / borrowed real serial. |
| **SPI "second serial"** | flash `0x13E00`, 32-byte slot | 17-char lot/batch-shaped string (§4) | **Unknown** (per-unit vs per-batch) | **Not served today.** Leave until classified. |

The genuine USB `iSerialNumber = "00"` is Confirmed four times over in ndeadly's descriptor dumps
(`descriptors.md` "Serial String Descriptor" → `00`) and matches this repo's descriptors
(`usb_descriptors.c:51`, `switch_gc.c:86`, `switch_pro2.c:65`, `switch_joycon2.c:183`). The
config-mode composite descriptor's `"000000000001"` (`usb_descriptors.c:249`) is a private,
never-console-facing value and is out of scope.

## 2. Evidence base — every genuine serial we hold

| # | Product | Serial (`0x13002`) | Source | Confidence |
|---|---|---|---|---|
| 1 | Joy-Con 2 **Left** | `HBW11030881989` | `dumps/SWITCH2_JOYCON_L_1.bin` (decoded here) | Confirmed |
| 2 | Joy-Con 2 **Right** | `HCW11029348578` | `dumps/SWITCH2_JOYCON_R_1.bin` (decoded here) | Confirmed |
| 3 | Pro Controller 2 (#a) | `HEJ71001121247` | ndeadly `memory_layout.md:65` (**= firmware value**) | Confirmed |
| 4 | Pro Controller 2 (#b) | `HEW70001504982` | `usbpcaptures/genuine_procon_2.pcapng` (owner's unit) | Confirmed |
| 5 | NSO GameCube (#1) | `HHW50001061937` | ndeadly / `switch_gc.c:310` | Confirmed |
| 6 | NSO GameCube (#2) | `HHW50001551810` | ndeadly / `nso-gc-spi-dump-analysis-2026-07-13.md` | Confirmed |

Two independent units for both Pro2 and GameCube — enough to separate the **fixed prefix** from the
**variable per-unit sequence** empirically, not just by analogy.

## 3. Structure of the `0x13002` serial (decoded)

Aligned by character position (1-indexed):

```
Pos:      1 2 3 4 5 6 7 8 9 10 11 12 13 14
JC2-L:    H B W 1 1 0 3 0 8  8  1  9  8  9
JC2-R:    H C W 1 1 0 2 9 3  4  8  5  7  8
Pro2 #a:  H E J 7 1 0 0 1 1  2  1  2  4  7
Pro2 #b:  H E W 7 0 0 0 1 5  0  4  9  8  2
GC  #1:   H H W 5 0 0 0 1 0  6  1  9  3  7
GC  #2:   H H W 5 0 0 0 1 5  5  1  8  1  0
```

| Field | Chars | Values seen | Meaning | Confidence |
|---|---|---|---|---|
| **Family** | 1 | `H` (all 6) | Switch-2-era accessory family marker | **Confirmed** |
| **Model code** | 1–2 | `HB`=JC2-L · `HC`=JC2-R · `HE`=Pro2 · `HH`=GC | Product identity (char 2 also encodes JC side: B=L, C=R) | **Confirmed** (both Pro2 units share `HE`; both JC differ only here) |
| **Plant letter** | 3 | `W` (5/6), `J` (Pro2 #a) | Assembly plant / line code | **Strong** — the two genuine Pro2 units differ here (`J` vs `W`), so it varies *within* a model → not part of the model code |
| **Plant digit** | 4 | JC=`1`, Pro2=`7`, GC=`5` | Plant / revision digit | **Hypothesis** — stable within each product's samples (both Pro2 = `7`), but only 1–2 samples per product; mirrors the documented Switch-1 "4th-char = factory" convention |
| **Line/period** | 5–7 | `110`/`110` (JC), `100`/`000` (Pro2), `000`/`000` (GC) | Production line or period | **Hypothesis** — GC pair is constant `000`; Pro2 pair differs (`100` vs `000`), so not fully fixed |
| **Unit sequence** | 8–14 | all differ | Per-unit production sequence (7 digits) | **Confirmed per-unit-varying** — both GC and both Pro2 units share their prefix and differ here |

**Robust takeaways** (what we can build on):
- Chars **1–2 are the model code** — Confirmed. Emit the correct one per personality.
- The **last 7 digits are a per-unit sequence** — Confirmed varying between same-model units.
  This is exactly the field a generator should fill.
- Chars **3–7** are a plant/line/period block that is *mostly* stable per product but not provably
  fixed at n≤2; treat the safest generation prefix as the **model code + observed plant bytes**,
  varying only the tail (§7).

This matches Nintendo's long-documented serial convention (letter model code + factory digit + long
production run) seen on Switch 1 hardware — consistent, but the precise per-field semantics beyond
"model code" and "per-unit tail" remain **Hypothesis** until more same-model dumps exist.

## 4. The second serial at `0x13E00` (lot/batch code)

A separate 32-byte slot holds a second string, shape `8 digits + 9 alphanumerics`:

| Product | `0x13E00` value | Source |
|---|---|---|
| Joy-Con 2 L | `25256051P601E6C15` | `SWITCH2_JOYCON_L_1.bin` (decoded here) |
| Joy-Con 2 R | `25256051Q401S4SVJ` | `SWITCH2_JOYCON_R_1.bin` (decoded here) |
| Pro2 (ndeadly) | `17152528LD1743101` | `memory_layout.md:81` ("Unknown serial") |
| GC (ndeadly) | `22253765Q01C0612M` | `nso-gc-spi-dump-analysis-2026-07-13.md:122` |

The Joy-Con L and R (a physically bundled pair) **share the 8-digit prefix `25256051`** and differ
only in the alphanumeric tail — strong evidence this is a **manufacturing lot/date code** where the
prefix is per-batch and the tail is per-unit. **PicoSwitch2 does not serve this field** (reads back
`0xFF`), and the console has never been observed reading it, so it is **out of scope** for
generation until (a) we confirm the console ever reads `0x13E00` and (b) we classify the tail as
per-unit vs per-batch. Documented here so it is never mistaken for the primary serial.

## 5. What PicoSwitch2 emits today (the problem)

| Personality | Emitted `0x13002` serial | Source | Issue |
|---|---|---|---|
| Pro Controller 2 | `HEJ71001121247` | `switch_pro2.c:252` (bytes `48 45 4A 37 …`) | **A real third party's unit serial** (ndeadly's, from `memory_layout.md`), identical on every PicoSwitch2 |
| NSO GameCube | `HHW50009999999` | `switch_gc.c:315` | Deliberately-fake `…9999999` tail; identical on every unit; obviously synthetic |
| Joy-Con 2 L | `HBW99999999999` | `ns2_joycon2_identity.c:8` | Fake all-`9` tail; identical on every unit |
| Joy-Con 2 R | `HCW99999999999` | `ns2_joycon2_identity.c:8,26` | Fake all-`9` tail; identical on every unit |

Three distinct issues:
1. **Not unique.** Two PicoSwitch2 dongles on the same host present identical serials. This is the
   same class of collision that already bit the project via `bcdDevice` — Windows keys its WinUSB
   binding cache on identity fields, and a genuine controller sharing our identity caused Code 28 /
   lost mappings (`switch_gc.c:59-76`). A unique serial is a second, cheaper axis of
   disambiguation.
2. **Pro2 reuses a real person's serial.** `HEJ71001121247` is a specific genuine unit's number.
   `COLOR-INVESTIGATION.md:85` even notes ours differs from the owner's own unit
   (`HEW70001504982`) "by design" — but "ours" is still *someone's* real serial, hardcoded and
   shared.
3. **Obviously synthetic where faked.** `…9999999` never occurs on genuine production hardware;
   it reads as fake to anyone inspecting the identity block.

The unused `platform_get_serial()` / `platform_get_unique_id()` declarations
(`platform.h:33,36` — **declared, never implemented, never called**) are the intended seam for
fixing all three at once.

## 6. Does the console validate the serial?

**No evidence it does. (Strong Evidence.)** The Pro2 and GameCube personalities are hardware-
validated for enumeration, pairing, and streaming (STATUS.md) **while emitting the placeholder
serials above**. A content check would have rejected `HHW50009999999` / `HBW99999999999`. So:

- The console treats `0x13002` as **opaque identity data it echoes/stores, not validates.**
- Therefore **"valid" means format-plausible**, not "accepted" — any 14-char `HB/HC/HE/HH…` string
  already works. The reason to generate is **uniqueness + indistinguishability**, not console
  acceptance.
- Residual unknown: whether the console ever *displays* or *deduplicates* by this serial (e.g.
  "you already registered this controller"). Untested — see §9.

## 7. Generation design (per-Pico, unique, format-valid)

### 7.1 Source of uniqueness

The RP2040/RP2350 exposes a **unique 64-bit board ID** from flash via
`pico_get_unique_board_id()` (`pico/unique_id.h`). It is:
- **Stable** across reboots and reflashes (it's the flash chip's ID), so a given dongle always
  presents the same serial — good for host caches and user trust.
- **Unique** per physical board — no coordination or storage needed.

**Step 1: implement the existing stub.** `platform_get_unique_id(uint8_t *buf, size_t len)` →
`pico_get_unique_board_id()`. This finally gives the declared seam a body.

### 7.2 Algorithm

```
serial14 = MODEL_CODE[personality]        // "HB" | "HC" | "HE" | "HH"   (chars 1-2, Confirmed)
         + PLANT[personality]             // e.g. "W1" | "W7" | "W5"      (chars 3-4, observed)
         + PERIOD[personality]            // e.g. "000"                    (chars 5-7, observed-ish)
         + digits7(hash(board_id, slot))  // chars 8-14, per-unit sequence (7 decimal digits)
```

- `hash(board_id, slot)` = a fixed reduction of the 64-bit board ID (plus a **slot index** when one
  Pico emits multiple controllers simultaneously, so each slot gets a distinct serial) into a
  7-digit decimal number, e.g. `(fnv1a(board_id ^ slot) % 10_000_000)` zero-padded to 7.
- Deterministic: same board + same slot → same serial, every boot.
- The prefix (chars 1–7) stays a **plausible, model-correct constant** (Confirmed model code +
  observed plant/period), so the whole string passes as a real serial; only the Confirmed per-unit
  tail varies.

### 7.3 Collision safety (the one honest tradeoff)

Because the console doesn't validate, a generated serial *could* in principle equal a real unit's
serial that shares the same 7-char prefix. Two options:

- **(A) Hash-only (max plausibility).** Fill all 7 tail digits from the board ID. Residual chance of
  matching any *specific* real unit ≈ **1 in 10⁷**, and matching a *co-present* real controller on
  the same host is what actually matters (astronomically unlikely). Recommended default.
- **(B) Reserved marker (hard non-collision).** Pin one tail position to a value real production
  never uses, and hash the remaining 6. Guarantees no collision but requires proof of an unused
  value — which we **don't have** yet (needs a wider survey of genuine serials, §9). Until then, (A)
  is the pragmatic choice and is already strictly better than today's shared placeholder.

This supersedes the current "deliberately-`9999`" policy: that policy achieved non-collision by
being *obviously fake*. Option (A) achieves *effective* uniqueness while looking genuine — a better
point on the same tradeoff, appropriate now that the goal is indistinguishability.

### 7.4 What NOT to change

- **USB `iSerialNumber` stays `"00"`** (§1). Genuine hardware shares it; uniqueness there is wrong.
- **`0x13E00` second serial stays unserved** (§4) until classified.
- **Body/button/grip colours, calibration, keys** — unrelated identity fields, out of scope.

## 8. Proposed architecture & phasing

```
pico_get_unique_board_id() ──▶ platform_get_unique_id()   (implement the stub)
        │
        ▼
serial_gen(personality, slot) ──▶ "HE" + "W7" + "000" + digits7(hash(id,slot))
        │                                 └── model ──┘  └plant┘ └period┘ └── per-unit ──┘
        ▼
inject at 0x13002 of each identity block:
  · switch_pro2.c   ns2_ctrl_identity / factory[0x13002]
  · switch_gc.c     switch_gc_ctrl_identity[2..15]
  · ns2_joycon2_identity.c  IDENTITY_LEFT[2..15] (+ right override)
```

**Phasing (each independently testable):**
- **Phase 1 — plumbing.** Implement `platform_get_unique_id()`; add a small `serial_gen` helper
  (model/plant/period tables + `digits7`); unit-test that it's deterministic and that L/R/Pro2/GC
  produce the right prefixes. No hardware needed.
- **Phase 2 — inject.** Replace the three hardcoded `0x13002` serials with `serial_gen(...)` output
  at identity-build time. Hardware test: Pro2/GC still enumerate, pair, and stream; two dongles now
  show distinct serials.
- **Phase 3 — multi-slot & polish.** Mix the slot index for simultaneous multi-controller setups;
  optionally expose the generated serial over the config CDC link for debugging; decide whether to
  adopt collision-safety option (B) if a reserved value is ever confirmed.

## 9. Unknowns & experiments (ranked)

| Unknown | Hypothesis | Test |
|---|---|---|
| Does the console dedupe/display by `0x13002` serial? | It stores but never validates; may show it in a "registered controllers" list | Emit two different generated serials from one Pico across sessions; watch console UI/pairing behaviour |
| Exact semantics of chars 3–7 (plant/line/period) | Plant letter+digit then line/period, per Switch-1 convention | Collect more same-model genuine dumps; diff fixed vs varying positions |
| Is there a value real serials never use (for collision-proof option B)? | Some tail range or check pattern is unused | Survey a corpus of genuine Switch 2 controller serials |
| `0x13E00` per-unit vs per-batch, and does the console read it? | Lot code: 8-digit prefix per-batch, tail per-unit; console likely ignores it | Compare a second same-batch unit; grep console-side captures for reads at `0x13E00` |
| Board-ID → digits mapping bias | FNV/mod is uniform enough for 7 digits | Generate across many synthetic IDs; check digit distribution |

## 10. Risks & notes

- **Output-only, zero console-acceptance risk.** The console already accepts placeholder serials
  (§6), so switching to generated ones cannot *lose* compatibility; worst case is cosmetic.
- **Determinism matters.** Keep the mapping fixed forever — a serial that changes across firmware
  versions would churn host caches and confuse users. Version the algorithm if it ever must change.
- **Personality scope.** Each personality emits only its own model code; never emit a Joy-Con code
  from Pro2, etc.
- **No firmware change in this document** — this scopes the work; nothing is implemented.

## 11. References

- `dumps/SWITCH2_JOYCON_L_1.bin`, `dumps/SWITCH2_JOYCON_R_1.bin` — genuine Joy-Con 2 SPI dumps;
  serials at `0x13002` and lot codes at `0x13E00` **decoded here** (§2, §4).
- `usbpcaptures/genuine_procon_2.pcapng` — owner's Pro Controller 2, serial `HEW70001504982`
  (`docs/experiments/2026-07-19-usb-command-ab-diff.md:46,121`).
- `nso-gc-refs/switch2_controller_research/memory_layout.md:64-65,81` — `0x13000` header,
  `0x13002` serial, `0x13E00` "Unknown serial".
- `docs/experiments/nso-gc-spi-dump-analysis-2026-07-13.md:110-135,230` — GC serial + `0x13E00`
  lot code, exclusion-policy reasoning.
- `nso-gc-refs/switch2_controller_research/descriptors.md` "Serial String Descriptor" → `00`
  (genuine USB `iSerialNumber`, all types).
- `src/switch_pro2/switch_pro2.c:252` (Pro2 `0x13002` = `HEJ71001121247`),
  `src/switch_gc/switch_gc.c:308-315` (GC fictitious `…9999999`),
  `src/ns2_joycon2_identity.c:5-32` (Joy-Con placeholder `…99999999999`).
- `src/bt_hid/platform/platform.h:33,36` — `platform_get_serial` / `platform_get_unique_id`
  (declared, unimplemented, unused — the intended seam).
- `src/switch_gc/switch_gc.c:59-76` — `bcdDevice` WinUSB-cache collision precedent (why identity
  uniqueness matters host-side).
- `COLOR-INVESTIGATION.md:85` — firmware Pro2 serial vs owner's genuine unit.

## 12. Documentation corrections noted while writing

- `docs/experiments/2026-07-19-usb-command-ab-diff.md:121` writes the firmware Pro2 serial as
  `HEJ7100112147` (13 chars — a digit dropped); the correct 14-char value is `HEJ71001121247`
  (`memory_layout.md:65`, `switch_pro2.c:252`). Flagged, not yet fixed.
