# Laboratory evidence index

Tracked index for `/lab-evidence/`, which is **gitignored** — too large for Git, but it must
survive a `build/` wipe. This file is the durable record of what is there, where it came from, and
what depends on it.

## Why this exists

Until 2026-08-21 this material lived inside `build/`, which is gitignored, disposable, and has zero
tracked files. A routine "clean the build folder" would have permanently destroyed roughly 900 MB of
capture evidence, including the only Pro Controller 2 audio packet capture in the project. The
invariant this index enforces:

> Deleting `build/` must never destroy primary research evidence.

`/lab-evidence/` follows the pattern already established for `/usbpcaptures/` and `/nso-gc-refs/`:
large or independently versioned material stays local and gitignored, with its provenance recorded
in tracked documentation.

## Classification key

- **Primary** — irreplaceable raw observation: captures, recordings, packet traces. Cannot be
  regenerated without the original hardware, controller firmware, and console state.
- **Derived** — analysis output computed from primary evidence. Regenerable in principle if the
  producing tool and inputs still exist.
- **Regenerable** — reports/plots that can be recreated cheaply from preserved inputs.
- **Disposable** — build/tool output kept only because it sits inside a preserved set.

## Contents

| Set | Class | Size | Provenance / purpose |
|---|---|---|---|
| `pro2-capture-audit/` | **Primary** (mixed) | 704 MB, 35 009 files | Genuine Pro Controller 2 audio and transport capture audit. Contains `audio.pcapng`, `genuine-stream4.wav`, `current-live-cadence.jsonl`, plus decode probes and vendored ffmpeg codec sources used during the investigation. The probe `.exe`/`.c` files inside are *disposable*; the captures are not. |
| `transient-motion-media/` | **Primary** | 191 MB, 4 files | 2026-08-01 live gyro/prefix screen recordings (`20260801-live-gyro-screen*.mkv`, `20260801-live-prefix-screen.mkv`, `live-display3.png`). Recordings of real console behaviour; the "transient" name reflects their original scratch intent, not their value. |
| `audio-analysis/` | Regenerable | 21 MB, 13 files | Spectrum/waveform PNGs for Pro Controller 2 and DualSense audio output. Recreatable from the captures above with the audio lab tooling. **Note:** `tools/audio_lab_analyze.py` does *not* write here — an earlier claim that it did was a substring match on the schema string `picoswitch2-audio-analysis/v1`, not a path. |
| `genuine-command-atlas.json` / `.md` | Derived | 108 KB | Reverse-engineered atlas of genuine controller commands. |
| `motion-fitment-check.json` | Derived | 226 KB | Genuine-vs-generated motion fitment comparison output. |
| `android-ntag215-corpus-audit.json` | Derived | 8 KB | NTAG215 corpus audit result. |
| `baseline-b.csv` | Derived | 12 KB | Audio/motion baseline series. |
| `command-atlas-smoke.md` | Derived | small | Smoke summary for the command atlas. |
| `JOYCON2-AUDIT.md`, `MOUSE-MODE.md` | Derived | small | Historical audit notes. `JOYCON2-AUDIT.md` is cited by `docs/archive/tooling-plan-through-2026-07-21.archived.md`. |
| `ds5-test-tone.opus` | Regenerable | 3 KB | Produced by `tools/generate_ds5_audio_tone.c`. |
| `host-tools/` | Disposable | 1 MB, 6 files | Probe binaries and `.log` output from local investigations. |

## Checksums for key primary and derived artifacts

Recorded at relocation so a later copy can be verified.

| File | SHA-256 |
|---|---|
| `pro2-capture-audit/audio.pcapng` | `7a668b3632f9bff3f7f57abe3f00746eb3294cee6a422cabc5bbebf74da810b0` |
| `pro2-capture-audit/genuine-stream4.wav` | `b1d1192f97b9c8358e1addc6afb163fa0de7fbc7bdea8973e882ae3d17c0aa09` |
| `genuine-command-atlas.json` | `ffd552d60d3c643f7b94f8fc814fb4a0ce148599364f0a37590c3e8104e1a3cf` |
| `motion-fitment-check.json` | `74017ec10e00a4f4994e31fdf09374725f412d17880c40b22cbdff8b0c283941` |
| `android-ntag215-corpus-audit.json` | `5a1e663b14ee53b0c9d149dae39cfaada176e7a5a2980a1c1cbe8c3136514e09` |
| `baseline-b.csv` | `8abd5f1c65a466ddc3e3dd2ffb2b1e627aae6a757383387f157173415444d66b` |

## What depends on it

No live document currently cites these paths — they were unreferenced leftovers, which is precisely
why they were at risk. They are retained because they are hardware observations that cannot be
reconstructed without the original setup, not because a document points at them.

If a future experiment record relies on one of these sets, cite it as
`lab-evidence/<set>` and add the citation to the table above.

## Retention

- **Primary** sets: keep indefinitely, or until the same observation is superseded by a better
  capture recorded under `dumps/experiments/`.
- **Derived** / **Regenerable**: keep while the conclusions they support are still current; safe to
  discard once a maintained experiment record carries the same result.
- **Disposable**: no retention claim.

Nothing here has been deleted. The relocation on 2026-08-21 moved `build/_evidence/` to
`/lab-evidence/` byte-for-byte.

## Where new evidence should go

- Small, structured, citable experiment output → `dumps/experiments/<date>-<scenario>/` (**tracked**).
- Large captures, recordings, or reference clones → `/lab-evidence/`, `/usbpcaptures/`, or
  `/nso-gc-refs/` (**gitignored**), indexed here.
- Never `build/`. That directory is disposable by definition.
