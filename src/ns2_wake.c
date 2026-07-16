#include "ns2_wake.h"

#include <stdio.h>
#include <string.h>

#include "bt/btstack/btstack_host.h"
#include "config.h"
#include "ns2_wake_protocol.h"
#ifdef NS2_PRO
#include "usb.h"
#endif

// A short USB detach occurs during an intentional personality change, while a
// real console sleep/reset leaves the host absent for much longer. The debounce
// prevents the former from becoming a wake request. The boot grace gives an
// awake console time to enumerate before a cold-started Pico assumes the host
// is sleeping (the dock briefly power-cycles the Pico during sleep entry).
#define NS2_WAKE_USB_INACTIVE_DEBOUNCE_MS 750
#define NS2_WAKE_BOOT_GRACE_MS 2000
#define NS2_WAKE_RETRY_MS 500

static config_wake_identity_t pending_identity;
static bool pending_valid;

// Core0 -> core1 USB-state publication. Naturally-aligned scalar accesses are
// atomic on RP2040/RP2350; an occasional one-tick mixed snapshot is harmless
// because inactivity must remain stable for 750 ms before it is acted upon.
static volatile bool usb_host_mounted;
static volatile bool usb_host_suspended;
static volatile uint32_t usb_state_changed_ms;

// Controller input and all automatic-wake state are owned by core1.
static volatile bool controller_input_pending;
static volatile uint32_t controller_input_ms;
static bool auto_wake_initialized;
static bool auto_wake_armed;
static bool auto_wake_sent;
static uint32_t auto_wake_boot_ms;
static uint32_t auto_wake_retry_ms;

void ns2_wake_pairing_reset(void) {
    memset(&pending_identity, 0, sizeof(pending_identity));
    pending_valid = false;
}

bool ns2_wake_pairing_stage(const uint8_t *pairing_data, size_t len,
                            uint16_t product_id,
                            const uint8_t controller_addr_wire[6]) {
    pending_valid = ns2_wake_parse_pairing_data(pairing_data, len, product_id,
                                                controller_addr_wire,
                                                &pending_identity);
    if (pending_valid) {
        printf("[NS2_WAKE] Staged USB pairing identity (%u host%s)\n",
               pending_identity.host_count,
               pending_identity.host_count == 1 ? "" : "s");
    } else {
        printf("[NS2_WAKE] Ignored malformed 0x15/01 pairing identity\n");
    }
    return pending_valid;
}

void ns2_wake_pairing_commit(void) {
    if (!pending_valid) return;
    config_store_wake_identity(&pending_identity);
    printf("[NS2_WAKE] Pairing finalised; wake identity queued for persistence\n");
    ns2_wake_pairing_reset();
}

bool ns2_wake_request(void) {
    config_wake_identity_t identity;
    if (!config_get_wake_identity(&identity)) {
        printf("[NS2_WAKE] No completed Switch 2 USB pairing identity available\n");
        return false;
    }

    uint8_t advertisements[CONFIG_WAKE_MAX_HOSTS][NS2_WAKE_ADV_LEN];
    for (uint8_t i = 0; i < identity.host_count; i++) {
        ns2_wake_build_advertisement(identity.product_id,
                                     identity.host_addr_wire[i],
                                     advertisements[i]);
    }

    // BTstack bd_addr_t is display order; Nintendo's pairing command carries
    // this address least-significant byte first.
    uint8_t advertiser_addr[6];
    for (int i = 0; i < 6; i++) {
        advertiser_addr[i] = identity.controller_addr_wire[5 - i];
    }

    bool started = btstack_host_start_wake_advertisement(
        advertiser_addr, advertisements, identity.host_count);
    if (started) auto_wake_sent = true;
    printf("[NS2_WAKE] Manual wake request %s\n",
           started ? "started" : "deferred/rejected while radio is busy");
    return started;
}

void ns2_wake_publish_usb_state(bool mounted, bool suspended, uint32_t now_ms) {
    if (mounted == usb_host_mounted && suspended == usb_host_suspended) return;
    usb_host_mounted = mounted;
    usb_host_suspended = suspended;
    usb_state_changed_ms = now_ms;
}

void ns2_wake_note_controller_input(bool non_neutral, uint32_t now_ms) {
    // Deliberately latch only real input. Controllers commonly reconnect after
    // the dock's sleep power-cycle and immediately send neutral reports; using
    // connection alone would wake the console again every time it went to sleep.
    if (non_neutral) {
        controller_input_ms = now_ms;
        controller_input_pending = true;
    }
}

void ns2_wake_service(uint32_t now_ms) {
    if (!auto_wake_initialized) {
        auto_wake_initialized = true;
        auto_wake_boot_ms = now_ms;
    }

#ifdef NS2_PRO
    if (g_usb_personality != USB_PERSONALITY_SWITCH2_PRO2) {
        auto_wake_armed = false;
        auto_wake_sent = false;
        controller_input_pending = false;
        return;
    }
#else
    (void)now_ms;
    controller_input_pending = false;
    return;
#endif

    bool host_awake = usb_host_mounted && !usb_host_suspended;
    if (host_awake) {
        if (auto_wake_armed || auto_wake_sent) {
            printf("[NS2_WAKE] USB host active; automatic wake re-armed for next sleep\n");
        }
        auto_wake_armed = false;
        auto_wake_sent = false;
        auto_wake_retry_ms = 0;
        controller_input_pending = false;
        return;
    }

    uint32_t inactive_since = usb_state_changed_ms;
    if (inactive_since == 0) inactive_since = auto_wake_boot_ms;
    if ((int32_t)(now_ms - (auto_wake_boot_ms + NS2_WAKE_BOOT_GRACE_MS)) < 0 ||
        (int32_t)(now_ms - (inactive_since + NS2_WAKE_USB_INACTIVE_DEBOUNCE_MS)) < 0) {
        return;
    }

    if (!auto_wake_armed) {
        auto_wake_armed = true;
        // Drop gameplay/the HOME press that put the console to sleep. Only an
        // input received after the USB-inactive debounce has fully elapsed is
        // a new wake intent. A cold-booted Pico applies the same rule after its
        // longer enumeration grace.
        uint32_t accept_input_after = inactive_since + NS2_WAKE_USB_INACTIVE_DEBOUNCE_MS;
        uint32_t boot_accept_after = auto_wake_boot_ms + NS2_WAKE_BOOT_GRACE_MS;
        if ((int32_t)(boot_accept_after - accept_input_after) > 0) {
            accept_input_after = boot_accept_after;
        }
        if (controller_input_pending &&
            (int32_t)(controller_input_ms - accept_input_after) < 0) {
            controller_input_pending = false;
        }

        printf("[NS2_WAKE] USB host inactive; waiting for controller input\n");
    }

    if (!controller_input_pending || auto_wake_sent ||
        (auto_wake_retry_ms != 0 && (int32_t)(now_ms - auto_wake_retry_ms) < 0)) {
        return;
    }

    printf("[NS2_WAKE] Controller input while host inactive; sending wake\n");
    if (ns2_wake_request()) {
        controller_input_pending = false;
        auto_wake_retry_ms = 0;
    } else {
        // HID setup and a wake request can briefly contend for the radio. Keep
        // the user's intent latched and retry at a bounded cadence.
        auto_wake_retry_ms = now_ms + NS2_WAKE_RETRY_MS;
    }
}
