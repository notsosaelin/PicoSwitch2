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
#include "config.h"
#include "remap.h"

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

// Switch output bit (report byte index + mask) for each remap destination.
// DST_NONE and any gap default to {0,0}, so applying them is a harmless no-op.
static const struct {
    uint8_t idx;
    uint8_t mask;
} DST_BIT[DST_COUNT] = {
    [DST_A] = {0, SWITCH_MASK_A},   [DST_B] = {0, SWITCH_MASK_B},
    [DST_X] = {0, SWITCH_MASK_X},   [DST_Y] = {0, SWITCH_MASK_Y},
    [DST_R] = {0, SWITCH_MASK_R},   [DST_ZR] = {0, SWITCH_MASK_ZR},
    [DST_L] = {2, SWITCH_MASK_L},   [DST_ZL] = {2, SWITCH_MASK_ZL},
    [DST_L3] = {1, SWITCH_MASK_L3}, [DST_R3] = {1, SWITCH_MASK_R3},
    [DST_MINUS] = {1, SWITCH_MASK_MINUS},     [DST_PLUS] = {1, SWITCH_MASK_PLUS},
    [DST_HOME] = {1, SWITCH_MASK_HOME},       [DST_CAPTURE] = {1, SWITCH_MASK_CAPTURE},
    [DST_DPAD_UP] = {2, SWITCH_MASK_DPAD_UP}, [DST_DPAD_DOWN] = {2, SWITCH_MASK_DPAD_DOWN},
    [DST_DPAD_LEFT] = {2, SWITCH_MASK_DPAD_LEFT}, [DST_DPAD_RIGHT] = {2, SWITCH_MASK_DPAD_RIGHT},
};

static inline void apply_dst(switch_pro_input_t *in, uint8_t dst) {
    if (dst < DST_COUNT)
        in->buttons[DST_BIT[dst].idx] |= DST_BIT[dst].mask;
}

// Map a Bluetooth vendor id to a controller platform family (selects which
// remap profile applies, and the button labels shown in the config UI).
static uint8_t family_from_vendor(uint16_t vid) {
    switch (vid) {
        case 0x054C:
            return FAMILY_PLAYSTATION;  // Sony
        case 0x045E:
            return FAMILY_XBOX;  // Microsoft
        case 0x057E:
            return FAMILY_NINTENDO;  // Nintendo
        default:
            return FAMILY_GENERIC;
    }
}

// Translate a bluepad32 gamepad into the Pro Controller wire format.
static void fill_input(const uni_gamepad_t *gp, switch_pro_input_t *in, uint8_t family) {
    memset(in, 0, sizeof(*in));

    // Gather active physical inputs (bluepad32 normalizes to Xbox positions),
    // then route each through the configurable remap to a Switch output.
    bool src[SRC_COUNT] = {false};
    src[SRC_SOUTH] = gp->buttons & BUTTON_A;
    src[SRC_EAST] = gp->buttons & BUTTON_B;
    src[SRC_WEST] = gp->buttons & BUTTON_X;
    src[SRC_NORTH] = gp->buttons & BUTTON_Y;
    src[SRC_L] = gp->buttons & BUTTON_SHOULDER_L;
    src[SRC_R] = gp->buttons & BUTTON_SHOULDER_R;
    src[SRC_ZL] = (gp->buttons & BUTTON_TRIGGER_L) || gp->brake > 64;
    src[SRC_ZR] = (gp->buttons & BUTTON_TRIGGER_R) || gp->throttle > 64;
    src[SRC_L3] = gp->buttons & BUTTON_THUMB_L;
    src[SRC_R3] = gp->buttons & BUTTON_THUMB_R;
    src[SRC_MINUS] = gp->misc_buttons & MISC_BUTTON_SELECT;
    src[SRC_PLUS] = gp->misc_buttons & MISC_BUTTON_START;
    src[SRC_HOME] = gp->misc_buttons & MISC_BUTTON_SYSTEM;
    src[SRC_CAPTURE] = gp->misc_buttons & MISC_BUTTON_CAPTURE;
    src[SRC_DPAD_UP] = gp->dpad & DPAD_UP;
    src[SRC_DPAD_DOWN] = gp->dpad & DPAD_DOWN;
    src[SRC_DPAD_LEFT] = gp->dpad & DPAD_LEFT;
    src[SRC_DPAD_RIGHT] = gp->dpad & DPAD_RIGHT;

    uint8_t map[SRC_COUNT];
    config_get_button_map(family, map);
    for (int s = 0; s < SRC_COUNT; s++)
        if (src[s])
            apply_dst(in, map[s]);

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
#define WIPE_FLASH_MS 1200  // duration of the fast "erasing" LED burst

static btstack_timer_source_t control_timer;
static uint32_t pairing_until_ms;  // 0 = locked; else window open until this time
static uint32_t wipe_until_ms;     // 0 = idle; else show the fast wipe flash until this time
static uint32_t control_tick;

static void open_pairing_window(uint32_t now_ms) {
    uni_bt_allowlist_set_enabled(false);  // lift the gate: admit any controller
    pairing_until_ms = now_ms + PAIRING_WINDOW_MS;
    logi("pico_switch: pairing window open\n");
}

// Triple-tap: drop every active controller, forget all bonds, and clear the
// allow-list. All three are needed: disconnect drops the live links (del_keys
// alone leaves them until they idle out); del_keys forgets the pairing keys; and
// clearing + enforcing the allow-list makes the dongle *decline* the controller's
// incoming reconnect attempts (it otherwise still pages us from memory).
static void wipe_all_devices(void) {
    static const bd_addr_t zero_addr = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; i++) {
        uni_hid_device_t *d = uni_hid_device_get_instance_for_idx(i);
        if (d && bd_addr_cmp(d->conn.btaddr, zero_addr) != 0)
            uni_hid_device_disconnect(d);
    }
    uni_bt_del_keys_unsafe();
    uni_bt_allowlist_remove_all();
    uni_bt_allowlist_set_enabled(true);
}

static void control_timer_handler(btstack_timer_source_t *ts) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // In config mode, don't poll BOOTSEL (gestures aren't wanted while configuring).
    bootsel_gesture_t gesture = g_usb_config_mode ? BOOTSEL_NONE : bootsel_poll(now);
    switch (gesture) {
        case BOOTSEL_DOUBLE_TAP:
            open_pairing_window(now);
            break;
        case BOOTSEL_TRIPLE_TAP:
            pairing_until_ms = 0;
            wipe_all_devices();
            wipe_until_ms = now + WIPE_FLASH_MS;
            logi("pico_switch: wiped saved Bluetooth devices\n");
            break;
        case BOOTSEL_HOLD:
            // Signal the USB core to re-enumerate as a CDC config serial device.
            g_usb_enter_config = true;
            logi("pico_switch: entering config mode\n");
            break;
        case BOOTSEL_NONE:
            break;
    }

    // Perform a pending settings flash-write (runs here on core1, parking core0).
    config_service_save();

    // Close an expired pairing window: re-enforce the allow-list (re-lock).
    if (pairing_until_ms && now >= pairing_until_ms) {
        uni_bt_allowlist_set_enabled(true);
        pairing_until_ms = 0;
    }

    // LED state (priority: config > wipe burst > pairing window > connected > idle).
    bool led;
#if defined(NS2_PRO) && defined(NS2_DIAG)
    // Diagnostic build: blink how far the console's init handshake progressed (0-10:
    // 3 = configured, 4 = EP0 identity, 5 = pairing challenge, 6 = pairing finalised,
    // 7 = calibration memory read, 8 = 0x11 query, 9 = feature configure/enable,
    // 10 = report selected -> streaming input).
    // stage 0 = slow continuous heartbeat (no host commands); otherwise N flashes spaced
    // 1.5 s apart, then a ~10 s gap before repeating, so the count is easy to read on
    // hardware. control_tick advances every CONTROL_TICK_MS (30 ms).
    extern volatile uint8_t g_ns2_stage;
    if (!g_usb_config_mode) {
        uint8_t st = g_ns2_stage;
        if (st == 0) {
            led = (control_tick / 25) % 2 == 0;  // ~0.75 s on/off = waiting
        } else {
            uint32_t per = 50;                          // 1.5 s between flashes
            uint32_t cycle = (uint32_t)st * per + 333;  // + ~10 s gap between phases
            uint32_t pos = control_tick % cycle;
            led = (pos < (uint32_t)st * per) && ((pos % per) < 10);  // 0.3 s flash each
        }
    } else
#endif
    if (g_usb_config_mode)
        led = (control_tick / 16) % 2 == 0;  // steady ~1s blink = config mode
    else if (wipe_until_ms && now < wipe_until_ms)
        led = (control_tick & 1);  // very fast flash = erasing pairings
    else if (pairing_until_ms)
        led = (control_tick / 4) % 2 == 0;  // fast blink = pairing window
    else if (connected_controllers > 0)
        led = true;  // solid
    else
        led = (control_tick % 66) < 3;  // brief flash every ~2 s = idle/locked
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

    // Locked by default: enforce the allow-list so only previously-paired
    // controllers may connect (incoming reconnects AND scanned discoveries are
    // both gated). Keep scanning on so bonded controllers reconnect on their own
    // — including BLE ones like Xbox. A BOOTSEL double-tap lifts enforcement for
    // a window to admit new controllers; keys persist across reboots.
    uni_bt_allowlist_set_enabled(true);
    uni_bt_start_scanning_and_autoconnect_unsafe();

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
    // Remember this controller so it's allowed to reconnect once the pairing
    // window closes (and across reboots — the allow-list persists in flash).
    uni_bt_allowlist_add_addr(d->conn.btaddr);
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

static uni_error_t pico_switch_platform_on_device_ready(uni_hid_device_t *d) {
    logi("pico_switch: device ready: %p\n", d);
    connected_controllers++;
    // LED is updated by the control timer.

    // Give controllers with an RGB lightbar their configured per-position color
    // so players can tell them apart (DualSense/DualShock 4 lack player LEDs).
    uint8_t idx = uni_hid_device_get_idx_for_instance(d);
    if (idx < 4 && d->report_parser.set_lightbar_color != NULL) {
        uint8_t rgb[3];
        config_get_lightbar(idx, rgb);
        d->report_parser.set_lightbar_color(d, rgb[0], rgb[1], rgb[2]);
    }

    return UNI_ERROR_SUCCESS;
}

static void pico_switch_platform_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl) {
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD)
        return;

    uint8_t idx = uni_hid_device_get_idx_for_instance(d);
    uint8_t family = family_from_vendor(uni_hid_device_get_vendor_id(d));
    switch_pro_input_t in;
    fill_input(&ctl->gamepad, &in, family);
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
