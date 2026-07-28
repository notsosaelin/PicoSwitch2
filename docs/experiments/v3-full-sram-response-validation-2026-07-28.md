# Figure-v3 full SRAM response validation — 2026-07-28

## Result

**Confirmed on a real Switch 2:** an untouched 2048-byte Kirby Air Riders dump reads and writes
through PicoSwitch2 without a tag originality signature override and without `key_retail.bin`.

The controller-facing `0x21` result is:

```
19-byte controller header + image[0x3C0..0x3FF] (all 64 SRAM bytes)
```

SRAM bytes `62..63` are the big-endian CRC-16/MCRF4XX over bytes `0..61`. They are per response,
not a fixed controller constant.

## Evidence

- Input image: the maintainer-owned `Kirby & Warp Star.bin`
- Input size / CRC32 / UID: `2048` / `40762971` / `04B4438ADB1F90`
- Input crypto: both amiibo HMACs valid against the owner's retail keys
- UART trace:
  [`../../dumps/v3-downloaded-kirby-warp-read-write-2026-07-28.jsonl`](../../dumps/v3-downloaded-kirby-warp-read-write-2026-07-28.jsonl)
- Written export:
  [`../../dumps/v3-downloaded-kirby-warp-written-2026-07-28.bin`](../../dumps/v3-downloaded-kirby-warp-written-2026-07-28.bin)
- Post-write CRC32: `C59933E5`
- Post-write crypto: HMAC valid; nickname `Test`, owner `Miles`
- Runtime diagnostics:
  `dev_cmd_staged=3`, `dev_results=3`, `write_chunks=6`, `write_commits=1`,
  `write_errors=0`
- Store state after the operation: generation `2`, dirty `true`, persisted `true`

The trace contains three independently reassembled 83-byte result buffers. In every one:

- type is `0x18`;
- UID is `04B4438ADB1F90`;
- bytes `[19..82]` equal the input image's entire `0x3C0..0x3FF` SRAM window;
- the final two bytes are `E5 11`.

The write path then receives six `0x14` chunks at offsets
`0, 76, 152, 228, 304, 380`, commits with `0x08`, reports state `0x05`, and stops without an
error or console crash.

## Root cause

PicoSwitch2 previously copied only `image[0x3C0..0x3DF]` into result bytes `[19..50]`, zeroed the
remainder, and hardcoded the genuine reference tag's final bytes `7A C4`. That value appeared
constant because both earlier captures used the same physical figure.

The published figure-v3 format already said the expected response is stored in the full SRAM
buffer:

- [xSke / pixl.js PR #381](https://github.com/solosky/pixl.js/pull/381)
- [xSke's technical write-up](https://github.com/N3evin/AmiiboAPI/issues/243#issuecomment-3591686037)
- [amiitool issue #17](https://github.com/socram8888/amiitool/issues/17)
- [weebo issue #9](https://github.com/bettse/weebo/issues/9)

Recomputing CRC-16/MCRF4XX settled the framing:

| SRAM response | Calculated / stored CRC |
|---|---:|
| Genuine captured Warp Star | `7A C4` |
| Downloaded Warp Star (all four riders) | `E5 11` |
| Downloaded Winged Star (all four riders) | `BB 21` |
| Downloaded Tank Star (all four riders) | `25 63` |
| Downloaded Shadow Star (all four riders) | `30 61` |

All 16 supplied dumps pass both amiibo HMAC verification and the full-SRAM CRC check. The CRC
grouping by machine is expected because the four rider variants reuse the same machine response.

The old rebuilt baseline contained the genuine first 32 SRAM bytes but retained the donor dump's
`E5 11` trailer. Its full response therefore did not validate; firmware's hardcoded `7A C4`
accidentally repaired only that one image on the wire. `tools/rebuild_v3_from_capture.py` now
extracts and stores the complete 64-byte result, producing CRC32 `8D337603`.

## Eliminated hypotheses

- A per-tag or UID-bound originality signature is required: **refuted**. The successful scan used
  no signature override; the 32-byte prefix field was zero.
- `key_retail.bin` is needed to import known-good dumps: **refuted**. Keys were used only offline
  to verify the evidence before and after the write.
- A downloaded dump must be re-signed onto the captured figure's UID/SRAM carrier: **refuted**.
- `7A C4` is an invariant controller trailer: **refuted**. It is the genuine sample's SRAM
  CRC-16/MCRF4XX.

## Implementation and automated coverage

- `ns2_v3_build_device_result()` now publishes the full 64-byte stored SRAM response.
- `ns2_amiibo_v3_sram_response_valid()` checks the response CRC without making older research
  files impossible to inspect.
- `tools/test_ns2_amiibo_v3.c` contains the genuine `7A C4` fixture and detects corruption in
  either half of the response; `tools/test_ns2_v3_compat_view.mjs` checks both crypto-equivalent
  compatibility views and valid SRAM CRCs across all 16 supplied dumps.
- `amiibo v3sig clear` is ordered before the generic hex parser and works as documented.
- All 53 compiled host tests pass.
- Pico 2 W builds successfully at the production 300 MHz profile; the install-reset marker remains
  page-aligned and outside persistence.

## Still pending

The written image remains intentionally dirty on the connected adapter. The separate production
portal **Sync amiibo** test should import this exact generation and acknowledge dirty state only
after IndexedDB persistence succeeds.
