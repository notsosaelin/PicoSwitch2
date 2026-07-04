#include "usb.h"

#include <stdint.h>

#include <tusb.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>

#include "switch_pro.h"
#include "report.h"
#include "config.h"

#ifdef NS2_PRO
#include "switch_pro2.h"
#endif

volatile bool usb_lockout_ready = false;
volatile bool g_usb_enter_config = false;
volatile bool g_usb_config_mode = false;

// Runs on core0. Owns the TinyUSB device stack. In normal mode it emulates the
// Pro Controller(s); on a BOOTSEL hold it re-enumerates as a USB CDC serial
// device and serves the configuration protocol.
void usb_core_task() {
    tusb_init();
#ifdef NS2_PRO
    ns2_init();
#else
    switch_pro_init();
#endif

    // Register as a multicore lockout victim so the Bluetooth core can briefly
    // park this core to sample the BOOTSEL button (shared flash CS pin).
    multicore_lockout_victim_init();
    usb_lockout_ready = true;

#ifndef NS2_PRO
    uint8_t report[64];
#endif

    while (1) {
        // Switch to configuration mode by re-enumerating as a CDC serial device.
        if (g_usb_enter_config && !g_usb_config_mode) {
            tud_disconnect();
            sleep_ms(100);
            g_usb_config_mode = true;
            tud_connect();
        }

        tud_task();

        if (g_usb_config_mode) {
            config_cdc_task();
            continue;
        }

        // While the console is asleep (USB suspended) the BT core keeps running,
        // so wake the console only when a controller button is actually pressed.
        if (tud_suspended()) {
            if (report_any_button_pressed())
                tud_remote_wakeup();
        }

#ifdef NS2_PRO
        ns2_task();
#else
        for (uint8_t i = 0; i < SWITCH_PRO_MAX_CONTROLLERS; i++) {
            if (tud_hid_n_ready(i)) {
                switch_pro_generate_report(i, report);
                tud_hid_n_report(i, 0, report, sizeof(report));
            }
        }
#endif
    }
}
