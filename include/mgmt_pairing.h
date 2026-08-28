#ifndef MGMT_PAIRING_H
#define MGMT_PAIRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Remote physical-controller pairing, management side.
 *
 * WHAT THIS IS NOT
 *
 * It is not a second pairing flow. The adapter has exactly one controller
 * pairing operation -- `open_pairing_window()` in ns2_bt_host.c, the same one
 * the BOOTSEL gesture drives -- and this module only decides what a management
 * client is allowed to ask of it and how the answer is worded. The radio
 * behaves identically whichever trigger started it, which is the whole point of
 * design section 32.
 *
 * WHY THE MANAGEMENT BONDING WINDOW IS SEPARATE
 *
 * `mgmt_accept_bonding()` used to read the SAME flag as controller pairing, so
 * opening a controller pairing window also admitted a new management bond.
 * Locally that is defensible -- someone holding the adapter pressed the button.
 * Remotely it is not: a "pair a controller" request travelling over the air
 * would open a window in which a DIFFERENT phone could claim the management
 * relationship. Phase 0 flagged this as a product decision owed before Phase 6
 * (audit section 7.3); the decision is to split them. Remote pairing opens
 * controller discovery and grants no management authority whatsoever.
 *
 * Everything here is pure C with no BTstack dependency, so the grammar, the
 * state names, the reason codes and the JSON are host-testable.
 */

#define MGMT_PAIRING_RESPONSE_CAPACITY 256u

// The window is firmware-enforced. Design section 34 asks for a bounded window
// and suggests 60 s; this reuses the established 30 s controller window instead
// of introducing a second duration, because section 32 requires ONE state
// machine and the physical gesture's behaviour must not change underneath the
// user. The client is told the real number rather than a nominal one.
#define MGMT_PAIRING_WINDOW_MS 30000u

typedef enum {
    // No pairing operation. The adapter is not discovering.
    MGMT_PAIRING_IDLE = 0,
    // The window is open and the adapter is looking for a controller.
    MGMT_PAIRING_DISCOVERING,
    // A candidate was found and is being connected/authenticated.
    MGMT_PAIRING_CONNECTING,
    // A controller completed pairing during this operation.
    MGMT_PAIRING_PAIRED,
    // The window closed without a controller completing.
    MGMT_PAIRING_TIMED_OUT,
    // A client asked for it to stop, or a local gesture superseded it.
    MGMT_PAIRING_CANCELLED,
    // The request was refused; see mgmt_pairing_reason_t.
    MGMT_PAIRING_BLOCKED,
} mgmt_pairing_state_t;

/*
 * Why a pairing operation ended the way it did.
 *
 * Machine-readable and stable: design section 40 requires the firmware to say
 * WHICH failure occurred rather than collapsing everything into "pairing
 * failed", and leaves the human wording to the app.
 */
typedef enum {
    MGMT_PAIRING_REASON_NONE = 0,
    // Discovery ran for the whole window and nothing completed.
    MGMT_PAIRING_REASON_NO_CONTROLLER,
    // Management is disabled, so no management-initiated operation may run.
    MGMT_PAIRING_REASON_MANAGEMENT_DISABLED,
    // A pairing operation is already running. Not an error; see the note on
    // mgmt_pairing_start_allowed().
    MGMT_PAIRING_REASON_BUSY,
    // Trust admission is locked out (a wipe is in progress, or the radio is
    // deliberately refusing new relationships).
    MGMT_PAIRING_REASON_LOCKED_OUT,
} mgmt_pairing_reason_t;

typedef enum {
    MGMT_PAIRING_INVALID = 0,
    MGMT_PAIRING_START,
    MGMT_PAIRING_STATUS,
    MGMT_PAIRING_CANCEL,
} mgmt_pairing_action_t;

/*
 * One operation, as reported to a client.
 *
 * `operation` is a generation, not a handle: it increments on every start, and
 * a status or cancel naming an older one is answered about the CURRENT
 * operation rather than being applied to it. That is what stops a reply the app
 * missed, or a command issued just before an adapter switch, from cancelling
 * something it never started (design section 65).
 */
typedef struct {
    uint32_t operation;
    uint8_t state;               // mgmt_pairing_state_t
    uint8_t reason;              // mgmt_pairing_reason_t
    uint32_t remaining_ms;       // 0 unless DISCOVERING/CONNECTING
    uint8_t candidates;          // controllers seen during this operation
} mgmt_pairing_snapshot_t;

// Parse the argument after the "pairing " command prefix: "start", "status",
// "cancel". No arguments; the window duration is the firmware's to choose, and
// letting a client shorten or extend it would make the physical gesture's
// behaviour depend on what an app asked for earlier.
bool mgmt_pairing_parse_command(const char *arg, mgmt_pairing_action_t *action);

const char *mgmt_pairing_state_name(mgmt_pairing_state_t state);
const char *mgmt_pairing_reason_name(mgmt_pairing_reason_t reason);

/*
 * May a management client start pairing right now?
 *
 * `already_active` covers BOTH a management-started operation and a window the
 * user opened with the BOOTSEL gesture, because they are the same window. A
 * second start is refused rather than silently re-arming: re-arming would
 * extend a window the user physically opened, and the honest answer to "start
 * pairing" when pairing is already running is that it already is.
 */
bool mgmt_pairing_start_allowed(bool management_enabled,
                                bool already_active,
                                bool pairing_locked_out,
                                mgmt_pairing_reason_t *reason);

/*
 * Remaining milliseconds, saturating at zero and never negative.
 *
 * Takes the deadline and now as unsigned milliseconds that may wrap; the
 * comparison is wrap-safe. A window whose deadline has passed reports 0 rather
 * than a huge number, which is what a client would otherwise render as "29 days
 * remaining" the moment the timer wrapped.
 */
uint32_t mgmt_pairing_remaining_ms(uint32_t deadline_ms, uint32_t now_ms,
                                   bool active);

// Format a snapshot. Never emits addresses or key material: a pairing status is
// progress, not an inventory.
size_t mgmt_pairing_format_status(const mgmt_pairing_snapshot_t *snapshot,
                                  char *output, size_t capacity);

#endif
