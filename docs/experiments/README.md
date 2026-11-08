# Experiments

Dated records of how PicoSwitch2's claims were tested: question, method, result,
confidence and what remains unknown. A record is authoritative for what was
observed in that experiment, not for the current implementation; see
[`STATUS.md`](../../STATUS.md) for that.

Preserve records, including negative results. Edit one only to fix provenance,
a dead link, or an explicit factual error. New records follow the template in
[CLAUDE.md](../../CLAUDE.md#experiment-documentation).

**Start with [`refuted-hypotheses.md`](refuted-hypotheses.md)**: the theories
that were disproven, why they were attractive, and the evidence against them.

## Motion

- [Genuine/generated motion hybrid harness — 2026-08-01](ds5-motion-hybrid-harness-2026-08-01.md)
- [DualSense length-`0x28` interleaved hardware A/B — 2026-08-01](ds5-pdu40-interleaved-hardware-2026-08-01.md)
- [Experiment Plan — Gyro RE by Differential Analysis](gyro-differential-re.md)
- [Experiment A — Differential USBPcap capture on the Steam PC (genuine vs. ours)](gyro-experiment-a-plan.md)
- [Experiment A — Results: Steam-path gyro root cause (report 0x05)](gyro-experiment-a-results.md)
- [Experiment C — Re-mining the existing USB capture (host→device side)](gyro-experiment-c-results.md)
- [Hardware Validation (2026-07-10) — Steam gyro fix + console gyro rewrite, on-hardware tests](gyro-hardware-validation-2026-07-10.md)
- [Native Pro Controller 2 Motion Passthrough — 2026-07-21](native-pro2-motion-passthrough-2026-07-21.md)
- [Length-`0x28` generation — evidence audit — 2026-07-31](pro2-0x28-evidence-audit-2026-07-31.md)
- [Pro Controller 2 reciprocal carrier-chart transition — 2026-07-29](pro2-carrier-chart-transition-2026-07-29.md)
- [Pro Controller 2 motion carrier — remaining unknown fields — 2026-07-31](pro2-carrier-unknown-fields-2026-07-31.md)
- [Pro Controller 2 IMU constants — audit without hardware — 2026-08-01](pro2-imu-constants-audit-2026-08-01.md)
- [Genuine Pro Controller 2 magnetic-stimulus matrix — 2026-07-29](pro2-magnetic-stimulus-matrix-2026-07-29.md)
- [Pro Controller 2 mode-3 split-carrier correction — 2026-07-29](pro2-mode3-carrier-prefix-2026-07-29.md)
- [Genuine Pro Controller 2 raw/native motion PCAP audit — 2026-07-29](pro2-raw-native-motion-pcap-2026-07-29.md)
- [Experiment Report: v2 Feature-Enable Matrix — GATT Ground Truth + First Observed Independent Motion-Correlated Data (2026-07-10)](sw2-v2-motion-block-discovery-2026-07-10.md)
- [Switch 2 Joy-Con 2 (R) native motion — decoded map v2](switch2_native_motion_map_DyCOOL.md)

## NFC and amiibo

- [Android Amiibo page parity — bounded evidence note — 2026-08-13](android-amiibo-page-parity-2026-08-13.md)
- [Generated amiibo rejected by Switch 2 (crypto is validated)](generated-amiibo-console-rejection-2026-07-26.md)
- [NFC feasibility and resource audit — 2026-07-25](nfc-feasibility-audit-2026-07-25.md)
- [Genuine Pro Controller 2 native NFC read through PicoSwitch2](pro2-native-nfc-read-2026-07-25.md)
- [Smash native NFC write-capture attempt](smash-native-nfc-write-attempt-2026-07-25.md)
- [Figure-v3 dynamic Air Riders allocation — 2026-07-28](v3-air-riders-dynamic-allocation-2026-07-28.md)
- [Figure-v3 Air Riders extended operation — 2026-07-28](v3-air-riders-extended-operation-2026-07-28.md)
- [Runbook — capturing a genuine controller reading a tag](v3-amiibo-genuine-capture-runbook.md)
- [Figure-v3 full SRAM response validation — 2026-07-28](v3-full-sram-response-validation-2026-07-28.md)
- [Virtual amiibo offline foundation — 2026-07-25](virtual-amiibo-foundation-2026-07-25.md)
- [Virtual Amiibo write, eject, re-present, and UART export validation](virtual-amiibo-lifecycle-validation-2026-07-25.md)
- [Virtual Amiibo persistence and clean/used library model — 2026-07-25](virtual-amiibo-persistence-and-library-model-2026-07-25.md)
- [Virtual Amiibo read validation](virtual-amiibo-read-validation-2026-07-25.md)
- [Virtual Amiibo first write attempt: console crash and USB RX fix](virtual-amiibo-write-crash-and-rx-fix-2026-07-25.md)
- [Virtual Amiibo write reconstruction from existing evidence](virtual-amiibo-write-evidence-2026-07-25.md)

## Companions and Controller Link

- [Android companion live pass — AYN Thor — 2026-08-13](android-companion-ayn-thor-live-2026-08-13.md)
- [AYN Thor Android controller bridge ADB audit — 2026-08-12](android-controller-ayn-thor-adb-audit-2026-08-12.md)
- [Retroid Pocket Classic Android controller bridge ADB audit — 2026-08-11](android-controller-retroid-pocket-classic-adb-audit-2026-08-11.md)
- [Should Android gameplay move from Classic HID to the Path C data plane?](android-gameplay-carrier-choice-2026-09-03.md)
- [Android fresh-pair bond transport, and a mixed-workload coexistence soak](android-le-bond-transport-and-coexistence-soak-2026-08-21.md)
- [Controller Link input published to a slot nothing reads](controller-link-console-slot-misroute-2026-08-21.md)
- [Controller Link cycling failure — 2026-08-22](controller-link-cycling-failure-2026-08-22.md)
- [Controller Link explicit-versus-Touch differential (2026-08-25)](controller-link-explicit-vs-touch-differential-2026-08-25.md)
- [KB/M resident upload stalls: the management reply was never notified](kbm-resident-upload-notify-stall-2026-08-31.md)
- [Why the Keyboard & Mouse page could not load: two wire defects](kbm-wire-pagination-data-loss-2026-08-30.md)
- [Touch analog trigger: why a downward pull cost the whole screen](touch-analog-trigger-vertical-travel-2026-08-26.md)
- [Can Windows be a Bluetooth Classic HID **Device**, like Android?](windows-classic-hid-device-feasibility-2026-09-02.md)
- [How Windows presents controllers, and which one Controller Link uses](windows-controller-enumeration-2026-09-02.md)
- [Analog "backlog": stick movement replaying after the player stops](windows-controller-link-analog-backlog-2026-09-03.md)
- [Windows Controller Link (Path C) — hardware qualification record](windows-controller-link-qualification-2026-09-03.md)
- [windows-hogp-bridge-feasibility — the Phase 6 gate](windows-hogp-bridge-feasibility-2026-08-31.md)
- [Windows HOGP: why the adapter never links to the PC](windows-hogp-legacy-advertising-2026-09-02.md)
- [Windows companion — the four remaining Phase 2 boundaries](windows-phase2-boundaries-2026-08-29.md)
- [Windows Phase 6 — Controller Link closeout](windows-phase6-closeout-2026-09-03.md)

## Bluetooth and management

- [A BLE keyboard is classified as a mouse](ble-keyboard-classified-as-mouse-2026-08-29.md)
- [Bluetooth wipe transport retention — 2026-08-20](bluetooth-wipe-transport-retention-2026-08-20.md)
- [Classic first-pair failure: inquiry re-admission destroys the candidate](classic-first-pair-readmission-2026-08-29.md)
- [Established BLE management GATT failure boundary (2026-08-25)](established-management-gatt-failure-2026-08-25.md)
- [Gate 2 identity-log hardware captures — 2026-07-12](gate2-identity-log-hardware-captures-2026-07-12.md)
- [Immediate GameCube-Mode Rumble Blast — Code Audit + PC Host Lab — 2026-07-14](gcusb-rumble-lab-2026-07-14.md)
- [In-band management ↔ controller coexistence failure](in-band-mgmt-coexistence-failure-2026-08-12.md)
- [Multi-peer bonded BLE reconnect failure](multi-peer-bonded-reconnect-2026-08-16.md)
- [Overnight autonomous investigation (2026-08-13)](overnight-investigation-2026-08-13.md)
- [Pico SDK 2.3.0 / BTstack 1.8.2 migration (2026-08-25)](pico-sdk-2.3-btstack-1.8.2-migration-2026-08-25.md)
- [Two controllers paired to the adapter both drive the console](two-controllers-both-drive-console-2026-09-06.md)
- [Console wake while the companion app is connected — investigation](wake-with-management-connected-2026-08-14.md)

## Audio

- [Companion audio downlink prototype (Windows first) — 2026-11-07](companion-audio-downlink-prototype-2026-11-07.md)
- [DualSense live-Opus first hardware pass — failed](2026-07-17-dualsense-live-opus-failure.md)

## USB personalities and controller hardware

- [Experiment — capture analyses: USB command A/B diff, and Joy-Con mouse mode / command `0x13`](2026-07-19-usb-command-ab-diff.md)
- [Config Mode CDC-Only Migration](config-cdc-only-migration-2026-07-25.md)
- [NSO GameCube Stage D — Steam Diagnosis and Report-0x05 Fix — 2026-07-13](gc-stage-d-steam-diagnosis-2026-07-13.md)
- [Experiment — Genuine Joy-Con 2 (L + R) SPI Flash Dump Analysis (2026-07-14)](joycon2-spi-dump-analysis-2026-07-14.md)
- [Dycool/NS-PC-Control audit (2026-07-12)](ns-pc-control-audit-2026-07-12.md)
- [NSO GameCube Controller Reference Audit](nso-gc-reference-repo-audit-2026-07-13.md)
- [NSO GameCube Controller — SPI Dump Analysis — 2026-07-13](nso-gc-spi-dump-analysis-2026-07-13.md)
- [NSO GameCube Controller — `rumble-procon-gccon.pcapng.gz` Decode — 2026-07-13](nso-gc-usb-capture-decode-2026-07-13.md)
- [Experiment — Genuine Pro Controller 2 SPI Flash Dump Analysis (2026-07-10)](spi-dump-analysis-2026-07-10.md)
- [Implementation-Level Feasibility Audit: `Dycool/Usb-relay-for-NS` for Genuine Console-Side Report-0x09 Capture (2026-07-10)](usb-relay-feasibility-audit-2026-07-10.md)

## Capture sets

- [`joycon2-captures/`](joycon2-captures/)
- [`nso-gc-captures/`](nso-gc-captures/)
