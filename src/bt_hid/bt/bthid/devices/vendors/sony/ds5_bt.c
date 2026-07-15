// ds5_bt.c - Sony DualSense Bluetooth Driver
// Handles PS5 DualSense controllers over Bluetooth
//
// Reference: https://controllers.fandom.com/wiki/Sony_DualSense
// BT reports have similar structure to USB but with different report IDs
// BT output reports require CRC32

#include "ds5_bt.h"
#include "ds5_output.h"
#include "bt/bthid/bthid.h"
#include "bt/transport/bt_transport.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/buttons.h"
#include "core/services/players/manager.h"
#include "core/services/players/feedback.h"
#include "platform/platform.h"
#include <string.h>
#include <stdio.h>

// Player LED colors (RGB values) - same as DS4
static const uint8_t PLAYER_COLORS[][3] = {
    {  0,   0,  64 },   // Player 1: Blue
    { 64,   0,   0 },   // Player 2: Red
    {  0,  64,   0 },   // Player 3: Green
    { 64,   0,  64 },   // Player 4: Pink/Fuchsia
    { 64,  64,   0 },   // Player 5: Yellow
    {  0,  64,  64 },   // Player 6: Cyan
    { 64,  32,   0 },   // Player 7: Orange
};

// Player LED patterns for DS5 (5 LEDs in a row)
// Pattern is a bitmask: bit 0=leftmost, bit 4=rightmost
static const uint8_t PLAYER_LED_PATTERNS[] = {
    0x04,   // Player 1: center LED (--*--)
    0x0A,   // Player 2: left+right of center (-*-*-)
    0x15,   // Player 3: outer + center (*-*-*)
    0x1B,   // Player 4: all but center (**-**)
    0x1F,   // Player 5: all LEDs (*****)
};

// ============================================================================
// DS5 CONSTANTS
// ============================================================================

// Report IDs
#define DS5_REPORT_BT_INPUT     0x31    // Full BT input report
#define DS5_REPORT_USB_INPUT    0x01    // USB input report (fallback)
#define DS5_REPORT_BT_OUTPUT    0x31    // BT output report

// ============================================================================
// DS5 REPORT STRUCTURE
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t x1, y1;         // Left stick
    uint8_t x2, y2;         // Right stick
    uint8_t l2_trigger;     // L2 analog
    uint8_t r2_trigger;     // R2 analog
    uint8_t counter;        // Report counter / sequence number

    struct {
        uint8_t dpad     : 4;   // Hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=released
        uint8_t square   : 1;
        uint8_t cross    : 1;
        uint8_t circle   : 1;
        uint8_t triangle : 1;
    };

    struct {
        uint8_t l1     : 1;
        uint8_t r1     : 1;
        uint8_t l2     : 1;
        uint8_t r2     : 1;
        uint8_t create : 1;     // Share/Create button
        uint8_t option : 1;
        uint8_t l3     : 1;
        uint8_t r3     : 1;
    };

    struct {
        uint8_t ps       : 1;   // 0x01 PlayStation button
        uint8_t tpad     : 1;   // 0x02 Touchpad click
        uint8_t mute     : 1;   // 0x04 Mute button
        uint8_t edge_res : 1;   // 0x08 (unused)
        uint8_t fn_left  : 1;   // 0x10 DualSense Edge: left function button
        uint8_t fn_right : 1;   // 0x20 DualSense Edge: right function button
        uint8_t paddle_left  : 1; // 0x40 DualSense Edge: left back paddle
        uint8_t paddle_right : 1; // 0x80 DualSense Edge: right back paddle
    };

    uint8_t reserved1;          // 4th button byte

    // Extended data for motion (matches Linux kernel hid-playstation.c)
    uint8_t reserved2[4];       // Timestamp/padding bytes
    int16_t gyro[3];            // x, y, z (pitch, yaw, roll)
    int16_t accel[3];           // x, y, z
    uint32_t sensor_timestamp;  // Sensor timestamp (0.33µs units)
    uint8_t  reserved3;         // Temperature / reserved
    // Touchpad: 4 bytes per finger (matches dualsense_touch_point in hid-playstation.c)
    // Finger 1 at struct offset 32, Finger 2 at struct offset 36
    struct { uint8_t tpad_f1_count : 7; uint8_t tpad_f1_down : 1; };
    uint8_t  tpad_f1_pos[3];
    struct { uint8_t tpad_f2_count : 7; uint8_t tpad_f2_down : 1; };
    uint8_t  tpad_f2_pos[3];
} ds5_input_report_t;

// DS5 BT output report for LED/rumble
typedef struct __attribute__((packed)) {
    uint8_t report_id;          // 0x31
    uint8_t seq_tag;            // Sequence tag (upper nibble)
    uint8_t tag;                // 0x10 for BT

    uint8_t valid_flag0;        // Feature flags
    uint8_t valid_flag1;
    uint8_t valid_flag2;

    uint8_t rumble_right;       // High frequency motor
    uint8_t rumble_left;        // Low frequency motor

    uint8_t headphone_volume;
    uint8_t speaker_volume;
    uint8_t mic_volume;

    uint8_t audio_flags;
    uint8_t mute_flags;

    uint8_t trigger_r[11];      // Right trigger haptics
    uint8_t trigger_l[11];      // Left trigger haptics

    uint8_t reserved1[6];

    uint8_t valid_flag3;

    uint8_t reserved2[2];

    uint8_t lightbar_setup;     // LED setup flag
    uint8_t led_brightness;
    uint8_t player_led;         // Player indicator LEDs

    uint8_t lightbar_r;
    uint8_t lightbar_g;
    uint8_t lightbar_b;
} ds5_bt_output_report_t;

// ============================================================================
// DRIVER DATA
// ============================================================================

typedef struct {
    input_event_t event;
    bool initialized;
    uint8_t activation_state;
    uint32_t activation_time;
    uint8_t output_seq;

    // Current feedback state (for change detection)
    uint8_t rumble_left;
    uint8_t rumble_right;
    uint8_t led_r, led_g, led_b;
    uint8_t player_led;

    // Touchpad swipe tracking
    uint16_t tpad_last_pos;
    bool tpad_dragging;
} ds5_bt_data_t;

static ds5_bt_data_t ds5_data[BTHID_MAX_DEVICES];

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static bool ds5_send_output(bthid_device_t* device, bool initialize_compat,
                            bool update_rumble, bool update_leds,
                            uint8_t rumble_left, uint8_t rumble_right,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t player_led)
{
    ds5_bt_data_t* ds5 = (ds5_bt_data_t*)device->driver_data;
    if (!ds5) return false;

    ds5_output_state_t state = {
        .initialize_compat = initialize_compat,
        .update_rumble = update_rumble,
        .update_leds = update_leds,
        .rumble_left = rumble_left,
        .rumble_right = rumble_right,
        .led_r = r,
        .led_g = g,
        .led_b = b,
        .player_leds = player_led,
    };

    // BTstack's transport consumes the transaction byte, extracts report ID
    // 0x31, and copies the remaining 77 bytes before this function returns.
    uint8_t buf[1 + DS5_BT_OUTPUT_REPORT_LEN];
    buf[0] = 0xA2;  // DATA | OUTPUT
    ds5_build_bt_output_report(ds5->output_seq, &state, &buf[1]);

    if (!bt_send_interrupt(device->conn_index, buf, sizeof(buf))) {
        printf("[DS5_BT] Output queue failed; retaining pending feedback\n");
        return false;
    }

    ds5->output_seq = (uint8_t)((ds5->output_seq + 1u) & 0x0Fu);
    if (update_rumble) {
        ds5->rumble_left = rumble_left;
        ds5->rumble_right = rumble_right;
    }
    if (update_leds) {
        ds5->led_r = r;
        ds5->led_g = g;
        ds5->led_b = b;
        ds5->player_led = player_led;
    }

    printf("[DS5_BT] Output queued: setup=%d rumble=%d L=%d R=%d leds=%d\n",
           initialize_compat, update_rumble, rumble_left, rumble_right, update_leds);
    return true;
}

// ============================================================================
// DRIVER IMPLEMENTATION
// ============================================================================

static bool ds5_match(const char* device_name, const uint8_t* class_of_device,
                      uint16_t vendor_id, uint16_t product_id, bool is_ble)
{
    (void)class_of_device;
    (void)is_ble;

    // VID/PID match (highest priority) - Sony vendor ID = 0x054C
    // DualSense = 0x0CE6, DualSense Edge = 0x0DF2
    if (vendor_id == 0x054C && (product_id == 0x0CE6 || product_id == 0x0DF2)) {
        return true;
    }

    // Name-based match (fallback if SDP query didn't return VID/PID)
    if (device_name) {
        if (strstr(device_name, "DualSense") != NULL) {
            return true;
        }
        if (strstr(device_name, "PS5 Controller") != NULL) {
            return true;
        }
    }

    return false;
}

static bool ds5_init(bthid_device_t* device)
{
    printf("[DS5_BT] Init for device: %s\n", device->name);

    // Find free data slot
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (!ds5_data[i].initialized) {
            init_input_event(&ds5_data[i].event);
            ds5_data[i].initialized = true;
            ds5_data[i].activation_state = 0;
            ds5_data[i].activation_time = 0;
            ds5_data[i].output_seq = 0;
            ds5_data[i].rumble_left = 0;
            ds5_data[i].rumble_right = 0;
            ds5_data[i].led_r = 0;
            ds5_data[i].led_g = 0;
            ds5_data[i].led_b = 64;  // Default blue
            ds5_data[i].player_led = PLAYER_LED_PATTERNS[0];
            ds5_data[i].tpad_last_pos = 0;
            ds5_data[i].tpad_dragging = false;

            ds5_data[i].event.type = INPUT_TYPE_GAMEPAD;
            ds5_data[i].event.transport = INPUT_TRANSPORT_BT_CLASSIC;
            ds5_data[i].event.dev_addr = device->conn_index;
            ds5_data[i].event.instance = 0;
            ds5_data[i].event.button_count = 14;
            ds5_data[i].event.has_motion = true;

            device->driver_data = &ds5_data[i];

            // Activation happens in task (state machine with delays)
            return true;
        }
    }

    return false;
}

static bool ds5_process_debug_done = false;

static void ds5_process_report(bthid_device_t* device, const uint8_t* data, uint16_t len)
{
    ds5_bt_data_t* ds5 = (ds5_bt_data_t*)device->driver_data;

    if (!ds5 || len < 1) return;

    // Debug: print first report received
    if (!ds5_process_debug_done) {
        printf("[DS5_BT] Process report: len=%d, data[0]=0x%02X\n", len, data[0]);
        ds5_process_debug_done = true;
    }

    uint8_t report_id = data[0];
    const uint8_t* report_data = NULL;
    uint16_t report_len = 0;

    if (report_id == DS5_REPORT_BT_INPUT && len >= 12) {
        // Full BT report: report_id (1) + header (1) = skip 2 bytes
        report_data = data + 2;
        report_len = len - 2;
    } else if (report_id == DS5_REPORT_USB_INPUT && len >= 10) {
        // USB-style report: skip just report_id
        report_data = data + 1;
        report_len = len - 1;
    } else {
        // Unknown report format
        printf("[DS5_BT] Unknown report: len=%d, data[0]=0x%02X\n", len, data[0]);
        return;
    }

    if (report_len < sizeof(ds5_input_report_t)) {
        return;
    }

    const ds5_input_report_t* rpt = (const ds5_input_report_t*)report_data;

    // Parse D-pad (hat format)
    bool dpad_up    = (rpt->dpad == 0 || rpt->dpad == 1 || rpt->dpad == 7);
    bool dpad_right = (rpt->dpad >= 1 && rpt->dpad <= 3);
    bool dpad_down  = (rpt->dpad >= 3 && rpt->dpad <= 5);
    bool dpad_left  = (rpt->dpad >= 5 && rpt->dpad <= 7);

    // Build button state (inverted: 0 = pressed in USBR convention)
    uint32_t buttons = 0x00000000;

    if (dpad_up)       buttons |= JP_BUTTON_DU;
    if (dpad_down)     buttons |= JP_BUTTON_DD;
    if (dpad_left)     buttons |= JP_BUTTON_DL;
    if (dpad_right)    buttons |= JP_BUTTON_DR;
    if (rpt->cross)    buttons |= JP_BUTTON_B1;
    if (rpt->circle)   buttons |= JP_BUTTON_B2;
    if (rpt->square)   buttons |= JP_BUTTON_B3;
    if (rpt->triangle) buttons |= JP_BUTTON_B4;
    if (rpt->l1)       buttons |= JP_BUTTON_L1;
    if (rpt->r1)       buttons |= JP_BUTTON_R1;
    if (rpt->l2)       buttons |= JP_BUTTON_L2;
    if (rpt->r2)       buttons |= JP_BUTTON_R2;
    if (rpt->create)   buttons |= JP_BUTTON_S1;
    if (rpt->option)   buttons |= JP_BUTTON_S2;
    if (rpt->l3)       buttons |= JP_BUTTON_L3;
    if (rpt->r3)       buttons |= JP_BUTTON_R3;
    if (rpt->ps)       buttons |= JP_BUTTON_A1;
    if (rpt->tpad)     buttons |= JP_BUTTON_A2;
    if (rpt->mute)     buttons |= JP_BUTTON_A3;

    // DualSense Edge extra inputs: same button byte as PS/touchpad/mute, upper bits
    // (0 on a standard DualSense, so this is safe to parse unconditionally). Offsets
    // per SDL's HIDAPI_DriverPS5 (0x10 FnL, 0x20 FnR, 0x40 left paddle, 0x80 right).
    // User mapping: back paddles -> Switch grips GL/GR (via L4/R4); the two front Fn
    // buttons -> Capture (screenshot) and C (via A2/A3).
    // Distinct bits (not touchpad A2 / mute A3) so the Fn buttons show as their own
    // inputs in the config view. Seam defaults: paddles->GL/GR, FnL->Capture, FnR->C.
    if (rpt->paddle_left)  buttons |= JP_BUTTON_L4;   // -> GL
    if (rpt->paddle_right) buttons |= JP_BUTTON_R4;   // -> GR
    if (rpt->fn_left)      buttons |= JP_BUTTON_A4;   // Fn L -> Capture (screenshot)
    if (rpt->fn_right)     buttons |= JP_BUTTON_A5;   // Fn R -> C

    // Update event
    ds5->event.buttons = buttons;

    // Analog sticks (HID convention: 0=up, 255=down)
    ds5->event.analog[ANALOG_LX] = rpt->x1;
    ds5->event.analog[ANALOG_LY] = rpt->y1;
    ds5->event.analog[ANALOG_RX] = rpt->x2;
    ds5->event.analog[ANALOG_RY] = rpt->y2;

    // Triggers
    ds5->event.analog[ANALOG_L2] = rpt->l2_trigger;
    ds5->event.analog[ANALOG_R2] = rpt->r2_trigger;

    // Motion data (DS5 has full 3-axis gyro and accel)
    // Check if we have enough data for motion
    if (report_len >= sizeof(ds5_input_report_t)) {
        ds5->event.has_motion = true;
        ds5->event.accel[0] = rpt->accel[0];
        ds5->event.accel[1] = rpt->accel[1];
        ds5->event.accel[2] = rpt->accel[2];
        ds5->event.gyro[0] = rpt->gyro[0];
        ds5->event.gyro[1] = rpt->gyro[1];
        ds5->event.gyro[2] = rpt->gyro[2];
    } else {
        ds5->event.has_motion = false;
    }

    // Battery: status byte at report_data[52] — bits 0-3 = level (0-10), bits 4-7 = status
    // Per Linux kernel hid-playstation.c: 0=discharging, 1=charging, 2=full, 0xa/0xb/0xf=error
    if (report_len > 52) {
        uint8_t raw = report_data[52];
        uint8_t level = raw & 0x0F;
        uint8_t status = (raw >> 4) & 0x0F;

        switch (status) {
            case 0x0:  // Discharging
                ds5->event.battery_level = (level > 10) ? 100 : level * 10 + 5;
                ds5->event.battery_charging = false;
                break;
            case 0x1:  // Charging
                ds5->event.battery_level = (level > 10) ? 100 : level * 10 + 5;
                ds5->event.battery_charging = true;
                break;
            case 0x2:  // Full
                ds5->event.battery_level = 100;
                ds5->event.battery_charging = false;
                break;
            default:   // 0xa=voltage/temp, 0xb=temp, 0xf=charge error
                ds5->event.battery_level = 0;
                ds5->event.battery_charging = false;
                break;
        }
    }

    // Touchpad (only in full 0x31 reports that include touch fields)
    if (report_len >= sizeof(ds5_input_report_t)) {
        uint16_t tx = ((rpt->tpad_f1_pos[1] & 0x0f) << 8) | (rpt->tpad_f1_pos[0] & 0xff);
        uint16_t ty = ((rpt->tpad_f1_pos[1] & 0xf0) >> 4) | ((rpt->tpad_f1_pos[2] & 0xff) << 4);
        uint16_t tx2 = ((rpt->tpad_f2_pos[1] & 0x0f) << 8) | (rpt->tpad_f2_pos[0] & 0xff);
        uint16_t ty2 = ((rpt->tpad_f2_pos[1] & 0xf0) >> 4) | ((rpt->tpad_f2_pos[2] & 0xff) << 4);

        // (Touchpad-halves were previously faked as L4/R4 back-paddles; the DualSense
        // Edge now reports real paddles above, so that hack is gone. Touchpad click
        // stays mapped to A2/Capture via the button parse.)

        // Touchpad swipe delta (horizontal)
        int8_t touchpad_delta_x = 0;
        if (!rpt->tpad_f1_down) {
            if (ds5->tpad_dragging) {
                int16_t delta = (int16_t)tx - (int16_t)ds5->tpad_last_pos;
                if (delta > 12) delta = 12;
                if (delta < -12) delta = -12;
                touchpad_delta_x = (int8_t)delta;
            }
            ds5->tpad_last_pos = tx;
            ds5->tpad_dragging = true;
        } else {
            ds5->tpad_dragging = false;
        }
        ds5->event.delta_x = touchpad_delta_x;

        // Touch coordinates for SInput pass-through
        ds5->event.touch[0].x = tx;
        ds5->event.touch[0].y = ty;
        ds5->event.touch[0].active = !rpt->tpad_f1_down;
        ds5->event.touch[1].x = tx2;
        ds5->event.touch[1].y = ty2;
        ds5->event.touch[1].active = !rpt->tpad_f2_down;
        ds5->event.has_touch = true;
    }

    // Submit to router
    router_submit_input(&ds5->event);
}

static void ds5_task(bthid_device_t* device)
{
    ds5_bt_data_t* ds5 = (ds5_bt_data_t*)device->driver_data;
    if (!ds5) return;

    uint32_t now = platform_time_ms();

    // State machine for activation with delays
    switch (ds5->activation_state) {
        case 0:  // Wait a moment then send initial LED
            ds5->activation_state = 1;
            ds5->activation_time = now;
            break;

        case 1:  // Wait 100ms then send daidr-compatible initialization + LEDs
            if (now - ds5->activation_time >= 100) {
                int player_idx = find_player_index(ds5->event.dev_addr, ds5->event.instance);
                int idx = (player_idx >= 0 && player_idx < 7) ? player_idx : 0;
                int pat_idx = (idx < 5) ? idx : idx % 5;
                // Match daidr/dualsense-tester's working first output report:
                // flag0=0xF7, flag1=0xF7, with the initial LED state in the same
                // packet. This replaces the LIGHT_OUT-only setup that left all
                // three tested controllers dark and without compatibility rumble.
                if (ds5_send_output(device, true, false, true, 0, 0,
                                    PLAYER_COLORS[idx][0], PLAYER_COLORS[idx][1], PLAYER_COLORS[idx][2],
                                    PLAYER_LED_PATTERNS[pat_idx])) {
                    ds5->activation_state = 2;
                    ds5->activation_time = now;
                }
            }
            break;

        case 2:  // Let the asynchronous HID/L2CAP setup report leave before live updates
            if (now - ds5->activation_time >= 30) ds5->activation_state = 3;
            break;

        case 3:  // Activated - monitor feedback system for rumble/LED updates
            {
                int player_idx = find_player_index(ds5->event.dev_addr, ds5->event.instance);
                if (player_idx >= 0) {
                    feedback_state_t* fb = feedback_get_state(player_idx);
                    if (!fb) break;

                    bool rumble_update = false;
                    bool led_update = false;
                    uint8_t r = ds5->led_r;
                    uint8_t g = ds5->led_g;
                    uint8_t b = ds5->led_b;
                    uint8_t player_led = ds5->player_led;
                    uint8_t rumble_left = ds5->rumble_left;
                    uint8_t rumble_right = ds5->rumble_right;

                    // Calculate player LED from pattern (like DS3)
                    // DS5 has separate player LED bar and RGB lightbar
                    uint8_t calc_player_led;
                    if (fb->led.pattern != 0) {
                        // Map feedback pattern to player number via PLAYER_LEDS[] lookup
                        int player_num = 0;
                        for (int p = 1; p <= 7; p++) {
                            if (fb->led.pattern == PLAYER_LEDS[p]) {
                                player_num = p - 1;
                                break;
                            }
                        }
                        int pat_idx = (player_num < 5) ? player_num : player_num % 5;
                        calc_player_led = PLAYER_LED_PATTERNS[pat_idx];
                    } else {
                        // Default to player index
                        int idx = (player_idx < 5) ? player_idx : player_idx % 5;
                        calc_player_led = PLAYER_LED_PATTERNS[idx];
                    }

                    // Check if player LED changed
                    if (calc_player_led != ds5->player_led) {
                        player_led = calc_player_led;
                        led_update = true;
                    }

                    // Check RGB lightbar from feedback
                    if (fb->led_dirty) {
                        if (fb->led.r != 0 || fb->led.g != 0 || fb->led.b != 0) {
                            // Host specified RGB color directly
                            r = fb->led.r;
                            g = fb->led.g;
                            b = fb->led.b;
                        } else if (fb->led.pattern != 0) {
                            // Use player color based on pattern via PLAYER_LEDS[] lookup
                            int player_num = 0;
                            for (int p = 1; p <= 7; p++) {
                                if (fb->led.pattern == PLAYER_LEDS[p]) {
                                    player_num = p - 1;
                                    break;
                                }
                            }
                            int color_idx = (player_num < 7) ? player_num : player_num % 7;
                            r = PLAYER_COLORS[color_idx][0];
                            g = PLAYER_COLORS[color_idx][1];
                            b = PLAYER_COLORS[color_idx][2];
                        } else {
                            // Default to player index color
                            int idx = (player_idx < 7) ? player_idx : player_idx % 7;
                            r = PLAYER_COLORS[idx][0];
                            g = PLAYER_COLORS[idx][1];
                            b = PLAYER_COLORS[idx][2];
                        }
                        player_led = calc_player_led;
                        led_update = true;
                    }

                    // Check rumble
                    if (fb->rumble_dirty) {
                        rumble_left = fb->rumble.left;
                        rumble_right = fb->rumble.right;
                        rumble_update = true;
                    }

                    // Also check if values changed (even without dirty flag)
                    if (rumble_left != ds5->rumble_left || rumble_right != ds5->rumble_right)
                        rumble_update = true;
                    if (r != ds5->led_r || g != ds5->led_g || b != ds5->led_b ||
                        player_led != ds5->player_led)
                        led_update = true;

                    if ((rumble_update || led_update) &&
                        ds5_send_output(device, false, rumble_update, led_update,
                                        rumble_left, rumble_right, r, g, b, player_led)) {
                        // Only consume feedback after BTstack has accepted the
                        // report. A failed STOP is therefore retried instead of
                        // leaving a previous motor command latched.
                        feedback_clear_dirty(player_idx);
                    }
                }
            }
            break;
    }
}

static void ds5_disconnect(bthid_device_t* device)
{
    printf("[DS5_BT] Disconnect: %s\n", device->name);

    ds5_bt_data_t* ds5 = (ds5_bt_data_t*)device->driver_data;
    if (ds5) {
        // Clear router state first (sends zeroed input report)
        router_device_disconnected(ds5->event.dev_addr, ds5->event.instance);
        // Remove player assignment
        remove_players_by_address(ds5->event.dev_addr, ds5->event.instance);

        init_input_event(&ds5->event);
        ds5->initialized = false;
    }
}

// ============================================================================
// DRIVER STRUCT
// ============================================================================

const bthid_driver_t ds5_bt_driver = {
    .name = "Sony DualSense",
    .transports = BTHID_TRANSPORT_CLASSIC,  // DualSense BT pairing is Classic BT, not BLE
    .match = ds5_match,
    .init = ds5_init,
    .process_report = ds5_process_report,
    .task = ds5_task,
    .disconnect = ds5_disconnect,
};

void ds5_bt_register(void)
{
    bthid_register_driver(&ds5_bt_driver);
}
