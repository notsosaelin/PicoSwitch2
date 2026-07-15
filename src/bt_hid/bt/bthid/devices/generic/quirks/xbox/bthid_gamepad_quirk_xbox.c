// bthid_gamepad_quirk_xbox.c - Xbox (base family) quirk profile for the generic BT gamepad
// driver. Covers any Xbox VID (0x045E) or name-matched Xbox pad over both BLE and Classic BT.
// Xbox Elite Series 2 (bthid_gamepad_quirk_xbox_elite2.c) shares this file's button-map
// selection and rumble implementation via non-static functions -- Elite 2 is still an Xbox pad
// for those purposes, it only adds its own extra back-paddle bytes on top.

#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"
#include "bt/bthid/bthid.h"  // bthid_send_output_report
#include <stddef.h>

// Xbox BT HID: buttons 1-15 with gaps at 3,6,9,10
// A=1, B=2, X=4, Y=5, LB=7, RB=8, View=11, Menu=12, Xbox=13, L3=14, R3=15
static const uint32_t XBOX_BUTTON_MAP[17] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B1,       // usage 1: A
    JP_BUTTON_B2,       // usage 2: B
    0,                  // usage 3: (pad)
    JP_BUTTON_B3,       // usage 4: X
    JP_BUTTON_B4,       // usage 5: Y
    0,                  // usage 6: (pad)
    JP_BUTTON_L1,       // usage 7: LB
    JP_BUTTON_R1,       // usage 8: RB
    0,                  // usage 9: (pad)
    0,                  // usage 10: (pad)
    JP_BUTTON_S1,       // usage 11: View
    JP_BUTTON_S2,       // usage 12: Menu
    JP_BUTTON_A1,       // usage 13: Xbox
    JP_BUTTON_L3,       // usage 14: L3
    JP_BUTTON_R3,       // usage 15: R3
    JP_BUTTON_A2,       // usage 16: Share (Series X/S)
};

// Xbox Classic BT: sequential buttons 1-15 (no gaps like BLE), different order from generic
static const uint32_t XBOX_SEQ_BUTTON_MAP[16] = {
    0,                  // usage 0: invalid
    JP_BUTTON_B1,       // usage 1: A
    JP_BUTTON_B2,       // usage 2: B
    JP_BUTTON_B3,       // usage 3: X
    JP_BUTTON_B4,       // usage 4: Y
    JP_BUTTON_L1,       // usage 5: LB
    JP_BUTTON_R1,       // usage 6: RB
    JP_BUTTON_S1,       // usage 7: Back/View
    JP_BUTTON_S2,       // usage 8: Start/Menu
    JP_BUTTON_L3,       // usage 9: L3
    JP_BUTTON_R3,       // usage 10: R3
    JP_BUTTON_A1,       // usage 11: Guide
    0,                  // usage 12
    0,                  // usage 13
    0,                  // usage 14
    JP_BUTTON_A2,       // usage 15: Share (Series X/S)
};

// Map buttons by HID usage number using descriptor-derived layout detection: Simulation
// Controls triggers (Brake/Accelerator) = Xbox BLE gap pattern; Generic Desktop triggers
// (Rx/Ry, i.e. no sim triggers) = Xbox Classic BT sequential layout. Shared by QUIRK_XBOX and
// QUIRK_XBOX_ELITE2 (non-static so that file can reuse it) -- Elite Series 2 doesn't have its
// own button table, only its own extra paddle bytes (see that quirk's file); the underlying
// face/shoulder/stick button layout is identical to any other Xbox pad on the same transport.
void xbox_select_button_map(uint8_t button_count, bool has_sim_triggers,
                             const uint32_t **out_map, uint8_t *out_size)
{
    (void)button_count;
    if (has_sim_triggers) {
        *out_map = XBOX_BUTTON_MAP;
        *out_size = sizeof(XBOX_BUTTON_MAP) / sizeof(XBOX_BUTTON_MAP[0]);
    } else {
        *out_map = XBOX_SEQ_BUTTON_MAP;
        *out_size = sizeof(XBOX_SEQ_BUTTON_MAP) / sizeof(XBOX_SEQ_BUTTON_MAP[0]);
    }
}

// Xbox Elite Series 2's own 4-paddle byte-19 extraction (bthid_gamepad_quirk_xbox_elite2.c) is
// reused here as a fallback: "Xbox + 20-byte report" rather than requiring the exact PID (which
// the BLE PnP query often fails to resolve, per that quirk's own comment) -- regular Xbox pads
// send 16-byte reports so they never hit this. Declared here, defined there, so the two quirks
// can't drift apart on the actual byte layout.
extern void xbox_elite2_extract_paddles(const uint8_t *data, uint16_t len, input_event_t *event);

static void extract_extra(const ble_report_map_t *map, const uint8_t *data, uint16_t len,
                          input_event_t *event)
{
    (void)map;
    if (len >= 20) {
        xbox_elite2_extract_paddles(data, len, event);
        return;
    }
    // Xbox extra byte: last byte, bit 0 (outside the HID buttons bitfield).
    // Series X/S Share (16-byte report, BLE) -> A2 ; Xbox One Back (Classic) -> S1.
    if (len > 0 && (data[len - 1] & 0x01)) {
        if (event->transport == INPUT_TRANSPORT_BT_BLE) {
            event->buttons |= JP_BUTTON_A2;
        } else {
            event->buttons |= JP_BUTTON_S1;
        }
    }
}

// Xbox rumble output report constants
#define XBOX_RUMBLE_REPORT_ID   0x03
#define XBOX_RUMBLE_MOTORS      0x03  // Enable strong (bit 1) + weak (bit 0) main motors

// Xbox controllers (VID 0x045E): Report ID 0x03, 8 bytes. Verified byte-for-byte 2026-07-12
// against the Linux xpadneo driver (atar-axis/xpadneo, xpadneo.h's
// xpadneo_rumble_report/xpadneo_rumble_data — the reference Xbox-BLE/BT HID driver):
// [0]=enable_actuators, [1]=left_trigger_magnitude, [2]=right_trigger_magnitude,
// [3]=strong_motor, [4]=weak_motor, [5]=pulse_sustain_10ms, [6]=pulse_release_10ms,
// [7]=loop_count
//
// Corrected 2026-07-15 (previously claimed xbox_bt.c/xbox_ble.c were dead/unregistered code and
// this was "the only Xbox rumble implementation in the tree" -- confirmed false by direct
// inspection while answering a project-owner question about driver architecture).
// `vendors/microsoft/xbox_bt.c`/`xbox_ble.c` ARE registered (`bthid_registry.c`'s
// `xbox_bt_register()`/`xbox_ble_register()`, gated behind `NS2_BT_ALL_DRIVERS`, which IS
// defined for the real build -- `CMakeLists.txt`) and each has its own real, independently
// working rumble implementation (`xbox_task()`/`xbox_ble_task()`) using this same byte format.
// Driver registration is first-match-wins and vendor drivers register before the generic
// fallback, so THIS implementation only actually runs for **Xbox Elite Series 2**
// (`xbox_ble_match()`/`xbox_bt_match()` explicitly exclude PID 0x0B05/0x0B22, "non-standard HID
// report layout," letting it fall through to the generic driver + QUIRK_XBOX_ELITE2) -- not for
// standard Xbox/Series controllers, which the dedicated files already own end-to-end. Kept here
// (not deleted) specifically because Elite 2 needs it; if a future standard-Xbox edge case ever
// falls through to this driver too (e.g. a device whose VID never resolves and whose name match
// also fails), it uses the identical byte format, so nothing behaves differently either way.
// Non-static (shared with QUIRK_XBOX_ELITE2, which uses the same rumble format).
bool xbox_send_rumble(uint8_t conn_index, uint8_t left, uint8_t right)
{
    bool stopping = (left == 0 && right == 0);
    uint8_t buf[8];
    // Confirmed 2026-07-14 by direct research into a documented, near-identical bug
    // class (atar-axis/xpadneo issue #400, "8BitDo Pro 2 non-stop rumble on connect",
    // fixed in commit 94ad82a): that bug was root-caused to the enable/motor-mask bits
    // staying set on a stop command, with only the magnitude zeroed -- some controllers'
    // firmware do not reliably treat "enabled, magnitude 0" as equivalent to "disabled".
    // This driver had exactly that shape (buf[0] was unconditionally XBOX_RUMBLE_MOTORS
    // regardless of amplitude) before this fix. Disabling the enable bits entirely on a
    // genuine stop is strictly more correct and costs nothing -- apply it regardless of
    // whether it's confirmed to be *this specific* project's trigger.
    buf[0] = stopping ? 0x00 : XBOX_RUMBLE_MOTORS;
    buf[1] = 0;                              // Left trigger magnitude (0: enable bits don't request trigger motors)
    buf[2] = 0;                              // Right trigger magnitude (same)
    buf[3] = ((uint16_t)left * 100) / 255;   // Strong motor (0-100)
    buf[4] = ((uint16_t)right * 100) / 255;  // Weak motor (0-100)
    // pulse_sustain_10ms: Confirmed 2026-07-14 by real hardware feedback to be the actual
    // bug, distinct from the enable-bits fix above. This was 0xFF (max, ~2550ms PER
    // TRIGGER) regardless of amplitude -- fine for a single isolated rumble command, but
    // real gameplay (Smash Bros) sends a rapid stream of brief, deliberately small/
    // textured rumble ticks, never a single one-shot trigger. Each of those legitimate
    // ticks was independently re-arming a ~2.55s hold with no release gap (buf[6]=0), so
    // any two ticks landing within 2.55s of each other (near-guaranteed for "textured"
    // gameplay rumble sent every few tens of ms) smeared into one continuous motor
    // engagement instead of a series of distinct short buzzes -- reported as "a powerful
    // continuous [rumble]" that "only stops if there's a transition screen where normally
    // nothing would send any rumble signal at all" (i.e. only when NO trigger arrives for
    // a full ~2.55s) and persisting briefly even right after pausing (whatever trigger
    // landed in the last ~2.55s before the pause keeps holding). Shortened to 0x05 (50ms)
    // so each individual trigger decays quickly on its own unless genuinely refreshed
    // faster than that by the host -- long enough to feel like a distinct tick, short
    // enough that a stream of separate small ticks reads as a texture, not one sustained
    // buzz. Still 0 on an explicit stop (see `stopping` above).
    buf[5] = stopping ? 0x00 : 0x05;
    buf[6] = 0x00;                           // pulse_release_10ms: no gap between pulses
    // loop_count: xpadneo sets 0xEB (235) to sustain ~10 minutes from one command
    // ("we pulse the motors for 60 minutes as the Windows driver does" —
    // rumble.c's rumble_worker()). This was 0x00 ("repeat: none"), which likely
    // stopped the motor after a single ~2.55s pulse_sustain burst and never
    // resumed, since this driver (correctly) only resends on an amplitude change.
    // Set to 0 on an explicit stop (see `stopping` above) so a stop command can never
    // itself be interpreted as "sustain this (zero) state for up to 10 minutes" --
    // it should mean "off, now," not "off, for a while." Kept at 0xEB for a genuine
    // trigger -- with pulse_sustain now shortened to 0x05, the worst-case total duration
    // if a trigger is somehow never followed by anything else is 235*50ms ~= 11.75s, not
    // ~10 minutes -- a much safer bound while this is still not fully confirmed.
    buf[7] = stopping ? 0x00 : 0xEB;
    return bthid_send_output_report(conn_index, XBOX_RUMBLE_REPORT_ID, buf, sizeof(buf));
}

const gamepad_quirk_t QUIRK_XBOX = {
    .name = "xbox",
    .select_button_map = xbox_select_button_map,
    .extract_extra = extract_extra,
    .send_rumble = xbox_send_rumble,
};
