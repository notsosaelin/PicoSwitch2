#include "usb.h"

#include <stdint.h>

#include <tusb.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>

#include "switch_pro.h"
#include "report.h"

volatile bool usb_lockout_ready = false;

// Runs on core0. Owns the TinyUSB device stack and the per-interface Pro
// Controller protocol state machine. Console commands arrive asynchronously via
// tud_hid_set_report_cb() (see usb_descriptors.c); here we service the stack and
// stream input / handshake reports on each interface's IN endpoint.
void usb_core_task() {
    tusb_init();
    switch_pro_init();

    // Register as a multicore lockout victim so the Bluetooth core can briefly
    // park this core to sample the BOOTSEL button (shared flash CS pin).
    multicore_lockout_victim_init();
    usb_lockout_ready = true;

    uint8_t report[64];

    while (1) {
        tud_task();

        // While the console is asleep (USB suspended) the BT core keeps running,
        // so wake the console only when a controller button is actually pressed.
        if (tud_suspended()) {
            if (report_any_button_pressed())
                tud_remote_wakeup();
        }

        for (uint8_t i = 0; i < SWITCH_PRO_MAX_CONTROLLERS; i++) {
            if (tud_hid_n_ready(i)) {
                switch_pro_generate_report(i, report);
                tud_hid_n_report(i, 0, report, sizeof(report));
            }
        }
    }
}
