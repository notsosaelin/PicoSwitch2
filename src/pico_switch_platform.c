#include <stdio.h>
#include <string.h>

#include <pico/cyw43_arch.h>
#include <pico/multicore.h>
#include <pico/time.h>
#include <btstack.h>
#include <uni.h>

#include "sdkconfig.h"
#include "uni_hid_device.h"
#include "uni_log.h"
#include "usb.h"
#include "report.h"
#include "switch_pro.h"
#include "bootsel.h"

// Sanity check
#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Pico W must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

static uint8_t connected_controllers;

// Map a bluepad32 axis (-512..511) to a 12-bit Switch stick value (0..4095).
static uint16_t convert_axis_12bit(int32_t axis, bool invert) {
    int32_t v = (axis + 512) * 4;  // -512..511 -> 0..4092
    if (v < 0)
        v = 0;
    if (v > SWITCH_STICK_MAX)
        v = SWITCH_STICK_MAX;
    if (invert)
        v = SWITCH_STICK_MAX - v;
    return (uint16_t)v;
}

// --- IMU passthrough scaling (tunable) ---
// bluepad32 reports motion as gyro = (deg/s * 1024) and accel = (G * 8192).
// We return IMU calibration with zero origin + standard sensitivity, so the
// console reads ~16 LSB per deg/s and 4096 LSB per G. Hence:
//   gyro raw  = (deg/s * 1024) / 64 ;  accel raw = (G * 8192) / 2
#define IMU_GYRO_DIV 64
#define IMU_ACCEL_DIV 2

static int16_t clamp16(int32_t v) {
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

// Forward the console's rumble request (decoded on the USB core) to the physical
// controller. Sends on amplitude change (throttled) and refreshes periodically
// while rumbling, to avoid flooding the Bluetooth link with output reports.
static uint8_t s_last_rumble[CONFIG_BLUEPAD32_MAX_DEVICES];
static uint32_t s_last_rumble_ms[CONFIG_BLUEPAD32_MAX_DEVICES];

static void forward_rumble(uni_hid_device_t *d, uint8_t idx, uint8_t amp) {
    if (idx >= CONFIG_BLUEPAD32_MAX_DEVICES || d->report_parser.play_dual_rumble == NULL)
        return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now - s_last_rumble_ms[idx];
    bool send = false;
    if (amp != s_last_rumble[idx] && elapsed >= 30)
        send = true;  // amplitude changed (throttled)
    else if (amp > 0 && elapsed >= 150)
        send = true;  // keep-alive refresh (duration below outlasts this interval)
    if (send) {
        d->report_parser.play_dual_rumble(d, 0 /* delay ms */, 250 /* duration ms */, amp, amp);
        s_last_rumble[idx] = amp;
        s_last_rumble_ms[idx] = now;
    }
}

static void make_neutral(switch_pro_input_t *in) {
    memset(in, 0, sizeof(*in));
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in->left_stick);
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, in->right_stick);
}

// Translate a bluepad32 gamepad into the Pro Controller wire format.
static void fill_input(const uni_gamepad_t *gp, switch_pro_input_t *in) {
    memset(in, 0, sizeof(*in));

    // Face buttons, position-faithful to a real Switch pad:
    // south->B, east->A, west->Y, north->X (bluepad32 normalizes to Xbox positions).
    if (gp->buttons & BUTTON_B)
        in->buttons[0] |= SWITCH_MASK_A;
    if (gp->buttons & BUTTON_A)
        in->buttons[0] |= SWITCH_MASK_B;
    if (gp->buttons & BUTTON_Y)
        in->buttons[0] |= SWITCH_MASK_X;
    if (gp->buttons & BUTTON_X)
        in->buttons[0] |= SWITCH_MASK_Y;

    // Shoulders and triggers (triggers may be buttons or analog brake/throttle).
    if (gp->buttons & BUTTON_SHOULDER_L)
        in->buttons[2] |= SWITCH_MASK_L;
    if (gp->buttons & BUTTON_SHOULDER_R)
        in->buttons[0] |= SWITCH_MASK_R;
    if ((gp->buttons & BUTTON_TRIGGER_L) || gp->brake > 64)
        in->buttons[2] |= SWITCH_MASK_ZL;
    if ((gp->buttons & BUTTON_TRIGGER_R) || gp->throttle > 64)
        in->buttons[0] |= SWITCH_MASK_ZR;

    // Stick clicks
    if (gp->buttons & BUTTON_THUMB_L)
        in->buttons[1] |= SWITCH_MASK_L3;
    if (gp->buttons & BUTTON_THUMB_R)
        in->buttons[1] |= SWITCH_MASK_R3;

    // Misc buttons
    if (gp->misc_buttons & MISC_BUTTON_SYSTEM)
        in->buttons[1] |= SWITCH_MASK_HOME;
    if (gp->misc_buttons & MISC_BUTTON_CAPTURE)
        in->buttons[1] |= SWITCH_MASK_CAPTURE;
    if (gp->misc_buttons & MISC_BUTTON_SELECT)
        in->buttons[1] |= SWITCH_MASK_MINUS;
    if (gp->misc_buttons & MISC_BUTTON_START)
        in->buttons[1] |= SWITCH_MASK_PLUS;

    // D-pad: individual bits in the 0x30 report (not a hat).
    if (gp->dpad & DPAD_UP)
        in->buttons[2] |= SWITCH_MASK_DPAD_UP;
    if (gp->dpad & DPAD_DOWN)
        in->buttons[2] |= SWITCH_MASK_DPAD_DOWN;
    if (gp->dpad & DPAD_LEFT)
        in->buttons[2] |= SWITCH_MASK_DPAD_LEFT;
    if (gp->dpad & DPAD_RIGHT)
        in->buttons[2] |= SWITCH_MASK_DPAD_RIGHT;

    // Analog sticks (12-bit; Y axes inverted for the Switch up-positive convention).
    uint16_t lx = convert_axis_12bit(gp->axis_x, false);
    uint16_t ly = convert_axis_12bit(gp->axis_y, true);
    uint16_t rx = convert_axis_12bit(gp->axis_rx, false);
    uint16_t ry = convert_axis_12bit(gp->axis_ry, true);
    switch_pro_pack_stick(lx, ly, in->left_stick);
    switch_pro_pack_stick(rx, ry, in->right_stick);

    // IMU passthrough. With zero-origin calibration a still controller reads 0
    // (no drift). The DualSense and Switch IMUs use different axis orientations;
    // this permutation + sign convention matches ndeadly/MissionControl's proven
    // DualSense -> Switch motion transform (bluepad32 gp->{accel,gyro}[0..2] are
    // the DualSense x,y,z in order):
    //   switch.x = -ds.z   switch.y = -ds.x   switch.z = +ds.y
    in->accel[0] = clamp16(-gp->accel[2] / IMU_ACCEL_DIV);
    in->accel[1] = clamp16(-gp->accel[0] / IMU_ACCEL_DIV);
    in->accel[2] = clamp16(gp->accel[1] / IMU_ACCEL_DIV);
    in->gyro[0] = clamp16(-gp->gyro[2] / IMU_GYRO_DIV);
    in->gyro[1] = clamp16(-gp->gyro[0] / IMU_GYRO_DIV);
    in->gyro[2] = clamp16(gp->gyro[1] / IMU_GYRO_DIV);
}

// Pairing + LED control, driven on core1 (where the CYW43 LED lives) by a single
// ~30 ms timer that also polls the BOOTSEL button.
//   - pairing window open      -> fast blink
//   - controller(s) connected  -> solid on
//   - idle / locked            -> brief flash every ~2 s
//
// Pairing model: scanning for NEW controllers is OFF unless a window is open;
// already-bonded controllers reconnect regardless (keys are persisted). A 10 s
// window opens at boot (so first-time setup works without the button) and on a
// BOOTSEL double-tap. Triple-tap wipes all saved devices.
#define CONTROL_TICK_MS 30
#define PAIRING_WINDOW_MS 10000

static btstack_timer_source_t control_timer;
static uint32_t pairing_until_ms;  // 0 = locked; else window open until this time
static uint32_t control_tick;

static void open_pairing_window(uint32_t now_ms) {
    uni_bt_start_scanning_and_autoconnect_unsafe();
    pairing_until_ms = now_ms + PAIRING_WINDOW_MS;
    logi("pico_switch: pairing window open\n");
}

static void control_timer_handler(btstack_timer_source_t *ts) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    switch (bootsel_poll(now)) {
        case BOOTSEL_DOUBLE_TAP:
            open_pairing_window(now);
            break;
        case BOOTSEL_TRIPLE_TAP:
            uni_bt_stop_scanning_unsafe();
            pairing_until_ms = 0;
            uni_bt_del_keys_unsafe();
            logi("pico_switch: wiped saved Bluetooth devices\n");
            break;
        case BOOTSEL_HOLD:
            // TODO (next milestone): enter USB config mode.
            logi("pico_switch: config-mode gesture (not yet implemented)\n");
            break;
        case BOOTSEL_NONE:
            break;
    }

    // Close an expired pairing window.
    if (pairing_until_ms && now >= pairing_until_ms) {
        uni_bt_stop_scanning_unsafe();
        pairing_until_ms = 0;
    }

    // LED state.
    bool led;
    if (pairing_until_ms)
        led = (control_tick / 4) % 2 == 0;  // fast blink (~240 ms period)
    else if (connected_controllers > 0)
        led = true;  // solid
    else
        led = (control_tick % 66) < 3;  // brief flash every ~2 s
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);

    control_tick++;
    btstack_run_loop_set_timer(ts, CONTROL_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

//
// Platform Overrides
//
static void pico_switch_platform_init(int argc, const char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    logi("pico_switch: init()\n");
    connected_controllers = 0;
    // Default (Xbox-position) mappings; face buttons are remapped to the Switch
    // layout in fill_input().
}

static void pico_switch_platform_on_init_complete(void) {
    logi("pico_switch: on_init_complete()\n");

    // Lower BR/EDR (e.g. DualSense) input latency: disallow sniff mode, which
    // otherwise lets the controller drop to a slow, low-power polling state.
    // (BLE controllers such as Xbox are unaffected.) Role switch stays enabled.
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH);

    // Persist bonds across reboots: do NOT delete keys here. Bonded controllers
    // reconnect automatically; new ones can only pair during a window. Open a
    // 10 s window at boot so first-time setup works without pressing BOOTSEL.
    open_pairing_window(to_ms_since_boot(get_absolute_time()));

    // Start the combined BOOTSEL + LED control timer.
    btstack_run_loop_set_timer_handler(&control_timer, control_timer_handler);
    btstack_run_loop_set_timer(&control_timer, CONTROL_TICK_MS);
    btstack_run_loop_add_timer(&control_timer);
}

static uni_error_t pico_switch_platform_on_device_discovered(bd_addr_t addr, const char *name, uint16_t cod, uint8_t rssi) {
    (void)addr;  // bd_addr_t is an array param; ARG_UNUSED's sizeof would warn
    ARG_UNUSED(name);
    ARG_UNUSED(cod);
    ARG_UNUSED(rssi);
    // Accept all discovered devices; non-gamepads are filtered later by class.
    return UNI_ERROR_SUCCESS;
}

static void pico_switch_platform_on_device_connected(uni_hid_device_t *d) {
    logi("pico_switch: device connected: %p\n", d);
}

static void pico_switch_platform_on_device_disconnected(uni_hid_device_t *d) {
    logi("pico_switch: device disconnected: %p\n", d);

    // The device index is no longer resolvable once disconnected, so reset all
    // slots to neutral to avoid a controller getting stuck in its last state.
    switch_pro_input_t neutral;
    make_neutral(&neutral);
    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; i++)
        set_global_gamepad_input(i, &neutral);

    if (connected_controllers > 0)
        connected_controllers--;
    // LED is updated by led_timer_handler().
}

// Per-player-position lightbar colors for controllers with an RGB light
// (DualSense / DualShock 4). Static defaults for now; the planned web config
// will make these user-selectable (R/G/B 0-255 per position).
static const uint8_t s_player_colors[4][3] = {
    {0x00, 0x00, 0xFF},  // P1 blue
    {0xFF, 0x00, 0x00},  // P2 red
    {0x00, 0xFF, 0x00},  // P3 green
    {0xFF, 0xC0, 0x00},  // P4 yellow
};

static uni_error_t pico_switch_platform_on_device_ready(uni_hid_device_t *d) {
    logi("pico_switch: device ready: %p\n", d);
    connected_controllers++;
    // LED is updated by led_timer_handler().

    // Give controllers with an RGB lightbar a per-position color so players can
    // tell them apart (DualSense/DualShock 4 lack player-number indicators).
    uint8_t idx = uni_hid_device_get_idx_for_instance(d);
    if (idx < 4 && d->report_parser.set_lightbar_color != NULL) {
        const uint8_t *c = s_player_colors[idx];
        d->report_parser.set_lightbar_color(d, c[0], c[1], c[2]);
    }

    return UNI_ERROR_SUCCESS;
}

static void pico_switch_platform_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl) {
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD)
        return;

    uint8_t idx = uni_hid_device_get_idx_for_instance(d);
    switch_pro_input_t in;
    fill_input(&ctl->gamepad, &in);
    set_global_gamepad_input(idx, &in);

    // Forward any rumble the console requested for this controller.
    forward_rumble(d, idx, report_get_rumble(idx));
}

static const uni_property_t *pico_switch_platform_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void pico_switch_platform_on_oob_event(uni_platform_oob_event_t event, void *data) {
    ARG_UNUSED(event);
    ARG_UNUSED(data);
}

//
// Entry Point
//
struct uni_platform *get_my_platform(void) {
    static struct uni_platform plat = {
        .name = "PicoSwitch",
        .init = pico_switch_platform_init,
        .on_init_complete = pico_switch_platform_on_init_complete,
        .on_device_discovered = pico_switch_platform_on_device_discovered,
        .on_device_connected = pico_switch_platform_on_device_connected,
        .on_device_disconnected = pico_switch_platform_on_device_disconnected,
        .on_device_ready = pico_switch_platform_on_device_ready,
        .on_oob_event = pico_switch_platform_on_oob_event,
        .on_controller_data = pico_switch_platform_on_controller_data,
        .get_property = pico_switch_platform_get_property,
    };

    return &plat;
}
