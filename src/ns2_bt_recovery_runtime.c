#include "ns2_bt_recovery_runtime.h"

#include <string.h>

#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#define RECOVERY_SCRATCH_MAGIC 0x42545243u /* BTRC */
#define RECOVERY_MAX_CONSECUTIVE_REBOOTS 3u
#define RECOVERY_STABLE_CLEAR_MS 60000u

static volatile bool reboot_pending;
static volatile uint8_t reboot_cause;
static volatile uint32_t reboot_requests;
static uint8_t boot_recovery_count;
static uint8_t last_boot_cause;
static bool reboot_suppressed;
static uint32_t initialized_ms;
static volatile uint32_t core1_heartbeat_sequence;
static volatile uint32_t core1_heartbeat_ms;
static volatile uint32_t control_tick_ms;
static volatile uint32_t control_tick_max_gap_ms;
// Packed escalation snapshot: [0] pending (this boot), [1] as read back from the
// previous boot. scratch[3] carries it across the reboot; the SDK owns
// scratch[4..7] for watchdog_reboot() itself.
static volatile uint32_t escalation_pending;
static uint32_t escalation_previous;

#define ESC_VALID_BIT      0x80000000u
#define ESC_SAT4(v)        ((uint32_t)((v) > 15u ? 15u : (v)))
#define ESC_SAT7(v)        ((uint32_t)((v) > 127u ? 127u : (v)))

static uint32_t escalation_pack(const ns2_bt_recovery_escalation_t *s)
{
    if (!s) return 0u;
    uint32_t packed = ESC_VALID_BIT;
    packed |= ((uint32_t)s->phase & 0x7u);
    packed |= ESC_SAT4(s->probes_sent) << 3;
    packed |= ESC_SAT4(s->probe_failures) << 7;
    packed |= ESC_SAT4(s->recovery_attempts) << 11;
    packed |= ESC_SAT7(s->uptime_s) << 15;
    packed |= (s->pairing_window_open ? 1u : 0u) << 22;
    packed |= (s->management_client ? 1u : 0u) << 23;
    packed |= (s->classic_link ? 1u : 0u) << 24;
    packed |= (s->ble_link ? 1u : 0u) << 25;
    packed |= (s->discovery_active ? 1u : 0u) << 26;
    return packed;
}

static void escalation_unpack(uint32_t packed,
                              ns2_bt_recovery_escalation_t *out)
{
    memset(out, 0, sizeof(*out));
    if ((packed & ESC_VALID_BIT) == 0u) return;
    out->valid = true;
    out->phase = (uint8_t)(packed & 0x7u);
    out->probes_sent = (uint8_t)((packed >> 3) & 0xFu);
    out->probe_failures = (uint8_t)((packed >> 7) & 0xFu);
    out->recovery_attempts = (uint8_t)((packed >> 11) & 0xFu);
    out->uptime_s = (uint8_t)((packed >> 15) & 0x7Fu);
    out->pairing_window_open = (packed & (1u << 22)) != 0u;
    out->management_client = (packed & (1u << 23)) != 0u;
    out->classic_link = (packed & (1u << 24)) != 0u;
    out->ble_link = (packed & (1u << 25)) != 0u;
    out->discovery_active = (packed & (1u << 26)) != 0u;
}

void ns2_bt_recovery_note_escalation(const ns2_bt_recovery_escalation_t *state)
{
    escalation_pending = escalation_pack(state);
}

void ns2_bt_recovery_runtime_init(void)
{
    initialized_ms = to_ms_since_boot(get_absolute_time());
    if (watchdog_hw->scratch[0] == RECOVERY_SCRATCH_MAGIC) {
        boot_recovery_count = (uint8_t)watchdog_hw->scratch[1];
        last_boot_cause = (uint8_t)watchdog_hw->scratch[2];
        escalation_previous = watchdog_hw->scratch[3];
    } else {
        boot_recovery_count = 0u;
        last_boot_cause = 0u;
        escalation_previous = 0u;
        watchdog_hw->scratch[0] = RECOVERY_SCRATCH_MAGIC;
        watchdog_hw->scratch[1] = 0u;
        watchdog_hw->scratch[2] = 0u;
        watchdog_hw->scratch[3] = 0u;
    }
    escalation_pending = 0u;
    reboot_pending = false;
    reboot_cause = 0u;
    reboot_requests = 0u;
    reboot_suppressed = false;
    core1_heartbeat_sequence = 0u;
    core1_heartbeat_ms = initialized_ms;
    control_tick_ms = initialized_ms;
    control_tick_max_gap_ms = 0u;
}

void ns2_bt_recovery_note_core1_activity(uint32_t now_ms)
{
    core1_heartbeat_ms = now_ms;
    core1_heartbeat_sequence++;
}

void ns2_bt_recovery_note_control_tick(uint32_t now_ms)
{
    uint32_t const previous = control_tick_ms;
    if (previous != 0u) {
        uint32_t const gap = now_ms - previous;
        if (gap > control_tick_max_gap_ms) control_tick_max_gap_ms = gap;
    }
    control_tick_ms = now_ms;
    ns2_bt_recovery_note_core1_activity(now_ms);
}

void ns2_bt_recovery_request_reboot(uint8_t cause)
{
    reboot_cause = cause;
    reboot_requests++;
    reboot_pending = true;
}

void ns2_bt_recovery_core0_service(void)
{
    uint32_t const now = to_ms_since_boot(get_absolute_time());
    if (boot_recovery_count != 0u &&
        (uint32_t)(now - initialized_ms) >= RECOVERY_STABLE_CLEAR_MS) {
        boot_recovery_count = 0u;
        watchdog_hw->scratch[1] = 0u;
    }

    if (!reboot_pending) return;
    reboot_pending = false;
    if (boot_recovery_count >= RECOVERY_MAX_CONSECUTIVE_REBOOTS) {
        reboot_suppressed = true;
        return;
    }

    watchdog_hw->scratch[0] = RECOVERY_SCRATCH_MAGIC;
    watchdog_hw->scratch[1] = (uint32_t)(boot_recovery_count + 1u);
    watchdog_hw->scratch[2] = reboot_cause;
    // Carry the escalation context across the reboot that is about to erase
    // every RAM counter explaining it.
    watchdog_hw->scratch[3] = escalation_pending;
    watchdog_reboot(0u, 0u, 10u);
}

void ns2_bt_recovery_runtime_get_diag(ns2_bt_recovery_runtime_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->reboot_pending = reboot_pending;
    out->reboot_suppressed = reboot_suppressed;
    out->consecutive_recovery_boots = boot_recovery_count;
    out->last_boot_cause = last_boot_cause;
    out->reboot_requests = reboot_requests;
    uint32_t const now = to_ms_since_boot(get_absolute_time());
    out->core1_heartbeat_sequence = core1_heartbeat_sequence;
    out->core1_heartbeat_age_ms = now - core1_heartbeat_ms;
    out->control_tick_age_ms = now - control_tick_ms;
    out->control_tick_max_gap_ms = control_tick_max_gap_ms;
    escalation_unpack(escalation_previous, &out->last_escalation);
}
