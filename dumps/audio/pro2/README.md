# Genuine Pro Controller 2 Audio Artifacts

These recordings and UART captures supported the headset-audio framing work documented in
[`../../../docs/switch2/pro2-headset-audio.md`](../../../docs/switch2/pro2-headset-audio.md).

The clean implementation result is the documented 240-byte, 20 ms Opus/CELT frame split into
ordered 120-byte `0x04` and `0x02` GATT writes. Earlier distorted recordings are retained as
negative evidence; filenames describe the experiment but do not imply a validated codec model.
