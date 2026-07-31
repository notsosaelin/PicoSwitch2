---
name: picoswitch2-protocol-lab
description: Design, capture, analyze, and preserve evidence-first PicoSwitch2 controller protocol experiments. Use for UART command tracing, command-surface reverse engineering, genuine-versus-emulated A/B tests, unknown Switch 2 commands or fields, capture fixture generation, and any request that would otherwise require repeated speculative firmware flashes.
---

# PicoSwitch2 protocol lab

## Establish the evidence boundary

Read `AGENTS.md`, `STATUS.md`, `PLAN.md`, `docs/README.md`,
`docs/LLM/current-context.md`, and the domain protocol reference. Run
`git status --short` and preserve unrelated worktree changes.

Treat direct console/UART captures and hardware results as primary evidence.
Do not use a third-party implementation as Switch 2 truth. Record a rejected
model in `docs/experiments/refuted-hypotheses.md`.

## Classify before changing code

Name the failing layer: USB framing, BLE transport, command state, codec,
controller-family translation, persistence, portal, or hardware lifecycle. If
the evidence cannot select a layer, improve the observation point first.

Before hardware, state:

1. hypothesis;
2. one variable;
3. expected wire/state difference;
4. pass/fail discriminator;
5. why offline data cannot answer it.

## Use the laboratory tools

Use `tools/PicoSwitch2Lab.psm1` for provenance, port discovery, UTF-8 output,
artifact hashes, and the `picoswitch2-lab/v1` manifest. Do not create another
serial-port selector.

Choose the narrow runner:

- Native motion or magnetic-reference work: `tools/motion_lab.ps1`.
- Speaker continuity or microphone prerequisites: `tools/audio_lab.ps1`.
- Captured command-`0x0D` update: `tools/firmware_lab.ps1`.
- NFC/amiibo: `tools/nfc_lab.ps1` and its NFC skill.

Convert a valid zero-loss capture into permanent fixtures:

```powershell
python tools/capture_to_fixture.py <capture.jsonl> `
  --name <fixture_name> --output-json <fixture.json> --output-c <fixture.h>
```

The generator rejects dropped/overwritten captures. Do not override loss by
hand-editing the fixture.

## Run one hardware action

Do not send live UART mutations until the maintainer confirms the expected
controller and personality are connected and ready. Never assume a COM port.
Let the runner discover it.

One action produces one artifact bundle. If two physical actions are required,
run two experiments. Do not mix a firmware change, different controller, and
different game state into one attributable result.

## Close the experiment

Turn the result into a golden fixture, fail-closed fixture, or permanent
negative result. Update the active protocol document and current-state files
only when behavior changed materially. State static tests, build results, and
hardware validation separately.

Never commit raw pairing secrets, user keys, proprietary firmware blobs, or
unreviewed per-unit identity data. Commit sanitized metadata and hashes.
