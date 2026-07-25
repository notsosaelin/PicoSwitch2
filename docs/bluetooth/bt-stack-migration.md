# Bluetooth Stack Migration — ✅ shipped (bluepad32 → BTstack/joypad-os)

**Status:** ✅ **Complete (2026-07-04).** bluepad32 is fully retired; the vendored **joypad-os
bthid** stack (`src/bt_hid/`) on BTstack/CYW43 is the sole BT layer. Per-vendor HID drivers expose
every button (Switch 2 GL/GR/C, DualSense Edge paddles/Fn, Xbox Elite paddles); input maps through
the `switch_pro_input_t` seam at `src/bt_hid/ns2_seam.c`.

This was the *planning* document for that migration. It is preserved verbatim (Phases 0–5,
architecture rationale, the `-DBT_STACK_JOYPAD` flag that no longer exists) in the archive — read it
only for history, not as current intent:

- Archived plan: [../archive/bt-stack-migration-2026-07.archived.md](../archive/bt-stack-migration-2026-07.archived.md)
- Current architecture: [PLAN.md](../../PLAN.md) "Architecture" + [src/bt_hid/README.md](../../src/bt_hid/README.md)
- Vendored driver research: [controller-research.md](controller-research.md)
