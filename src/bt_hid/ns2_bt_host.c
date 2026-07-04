// ns2_bt_host.c — core1 Bluetooth bring-up for the BT_STACK_JOYPAD build.
//
// Replaces bluepad32's uni_init() path (main.c bluepad_core_task) with the
// joypad-os bthid stack: register the HID drivers, bring up BTstack + the HID
// host over the CYW43 transport, and run a periodic control timer that drives
// the stack, the LED, and the BOOTSEL pairing/wipe/config gestures — matching
// the behavior of pico_switch_platform.c's control_timer_handler, but on the
// bt_* transport API instead of bluepad32. Only compiled under -DBT_STACK_JOYPAD.

#include <string.h>

#include <btstack_run_loop.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>

#include "bt/transport/bt_transport.h"
#include "bt/bthid/bthid.h"
#include "bt/bthid/bthid_registry.h"
#include "bt/btstack/btstack_host.h"

#include "bootsel.h"
#include "config.h"
#include "usb.h"

// The CYW43 transport instance (src/bt_hid/bt/transport/bt_transport_cyw43.c).
// Its init() does cyw43_arch_init + btstack_cyw43_init + HID-host power-on.
extern const bt_transport_t bt_transport_cyw43;

#define CONTROL_TICK_MS   30
#define PAIRING_WINDOW_MS 10000
#define WIPE_FLASH_MS     1200

static btstack_timer_source_t control_timer;
static uint32_t control_tick;
static uint32_t pairing_until_ms;  // 0 = locked; else scan window open until this time
static uint32_t wipe_until_ms;     // 0 = idle; else show the fast wipe flash until this time

#if defined(NS2_PRO) && defined(NS2_DIAG)
extern volatile uint8_t g_ns2_stage;  // USB handshake progress (core0), blinked here
#endif

static void open_pairing_window(uint32_t now_ms) {
    bt_set_pairing_mode(true);  // start scanning for new controllers
    pairing_until_ms = now_ms + PAIRING_WINDOW_MS;
}

static void wipe_all_devices(void) {
    btstack_host_disconnect_all_devices();
    btstack_host_delete_all_bonds();
}

static void control_timer_handler(btstack_timer_source_t *ts) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // BOOTSEL gestures (suppressed in config mode).
    bootsel_gesture_t gesture = g_usb_config_mode ? BOOTSEL_NONE : bootsel_poll(now);
    switch (gesture) {
        case BOOTSEL_DOUBLE_TAP:
            open_pairing_window(now);
            break;
        case BOOTSEL_TRIPLE_TAP:
            pairing_until_ms = 0;
            wipe_all_devices();
            wipe_until_ms = now + WIPE_FLASH_MS;
            break;
        case BOOTSEL_HOLD:
            g_usb_enter_config = true;
            break;
        case BOOTSEL_NONE:
        default:
            break;
    }

    // Pending settings flash-write (runs here on core1, parking core0).
    config_service_save();

    // Close an expired pairing window (stop scanning).
    if (pairing_until_ms && now >= pairing_until_ms) {
        bt_set_pairing_mode(false);
        pairing_until_ms = 0;
    }

    // Service the Bluetooth stack + the per-device driver tasks (rumble, etc.).
    bt_task();
    bthid_task();

    // LED state (priority: config > wipe burst > pairing window > connected > idle),
    // identical semantics to the bluepad32 platform.
    bool led;
#if defined(NS2_PRO) && defined(NS2_DIAG)
    if (!g_usb_config_mode) {
        uint8_t st = g_ns2_stage;
        if (st == 0) {
            led = (control_tick / 25) % 2 == 0;  // ~0.75 s heartbeat = waiting
        } else {
            uint32_t per = 50, cycle = (uint32_t)st * per + 333, pos = control_tick % cycle;
            led = (pos < (uint32_t)st * per) && ((pos % per) < 10);  // N flashes, 1.5 s apart
        }
    } else
#endif
    if (g_usb_config_mode)
        led = (control_tick / 16) % 2 == 0;  // steady ~1 s blink = config mode
    else if (wipe_until_ms && now < wipe_until_ms)
        led = (control_tick & 1);  // very fast flash = erasing pairings
    else if (pairing_until_ms)
        led = (control_tick / 4) % 2 == 0;  // fast blink = pairing window
    else if (bt_get_connection_count() > 0)
        led = true;  // solid = controller connected
    else
        led = (control_tick % 66) < 3;  // brief flash every ~2 s = idle
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);

    control_tick++;
    btstack_run_loop_set_timer(ts, CONTROL_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

// core1 entry (launched from main.c under BT_STACK_JOYPAD).
void ns2_bt_core_task(void) {
    // Register the HID drivers first, then bring up BTstack + the HID host. The
    // CYW43 transport's init() performs cyw43_arch_init + btstack_cyw43_init and
    // powers on the controller, so no separate cyw43_arch_init() is needed here.
    bthid_registry_init();
    bt_init(&bt_transport_cyw43);

    btstack_run_loop_set_timer_handler(&control_timer, control_timer_handler);
    btstack_run_loop_set_timer(&control_timer, CONTROL_TICK_MS);
    btstack_run_loop_add_timer(&control_timer);

    btstack_run_loop_execute();  // does not return
}
