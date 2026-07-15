# Switch 2 Rich-Report Experiments

Last updated: 2026-07-15

The optional v2 experiment matrix in `btstack_host.c` is capture instrumentation, not shipping
connection policy. It compares feature masks, calibration reads, subscription order, and one
unconfirmed descriptor write while recording raw traffic.

## Unconfirmed descriptor handle

The reference viewer computes a target from `input_handle + 3`. Given observed value,
declaration, and CCC handles, `0x000C` is the only unassigned handle in the relevant gap and may be
a custom descriptor attached to the `0x000A` input characteristic. This is a reasoned address for
an experiment, not a confirmed report-rate control and not evidence that it enables a richer
report format.

The code deliberately uses a write-with-response so the capture records a definitive ATT status.
Do not enable this path by default or rename the handle as a settled protocol fact until full GATT
discovery or a successful controlled write identifies its UUID and purpose.

## Experiment variants

The matrix isolates:

- control feature masks;
- `0xFF` masks;
- the candidate handle write;
- the reference calibration-read sequence;
- the combined sequence with deferred CCC subscription.

Results and raw evidence belong in `docs/experiments/`; promoted protocol conclusions belong in
`docs/switch2/ble-controller-protocol-inventory.md`.
