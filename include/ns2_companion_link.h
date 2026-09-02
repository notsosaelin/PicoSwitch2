// ns2_companion_link.h - Windows Controller Link data plane (Path C).
//
// WHY THIS CARRIER EXISTS
//
// Android's Controller Link is a BR/EDR HID Device: the phone registers a HID
// application and the adapter connects to it as HID host, while a separate BLE
// ACL carries management. Two transports, one peer, no collision.
//
// Windows has no Classic HID Device role at any supported layer -- no user-mode
// L2CAP server, no WinRT HID device class, and PSM 0x11/0x13 reserved by
// bthport.sys even from a KMDF profile driver
// (docs/experiments/windows-classic-hid-device-feasibility-2026-09-02.md).
// Substituting BLE HOGP for the second leg put two LE relationships on one
// identity and the controller refused it with 0x0B ACL Connection Already
// Exists (docs/experiments/windows-hogp-legacy-advertising-2026-09-02.md).
//
// So Windows carries controller state over the management ACL it ALREADY has,
// on a dedicated binary characteristic pair beside the JSON command channel.
// No second ACL exists, so the collision cannot occur.
//
// WHAT THIS IS, AND WHAT IT IS NOT
//
// This is a FIRST-CLASS COMPANION INPUT SOURCE whose wire payload happens to
// reuse the canonical bridge report format. It is not a Bluetooth HID device
// and must never pretend to be one.
//
//     Companion Link GATT frame
//         -> Path C frame validation (version, opcode, sequence)
//         -> canonical v2 report decode (shared semantics)
//         -> explicit COMPANION input source
//         -> existing normalized state / arbiter / bridge pipeline
//
// Reused deliberately: the canonical v2 report format and its offsets, the
// normalized input_event_t, arbiter semantics, and the feedback encoding. NOT
// faked, ever: a bthid connection or slot, HID connection handles, Classic HID
// lifecycle, pairing state, or a physical-controller transport identity.
//
// That boundary is not cosmetic. Source ownership, stale-input neutralization,
// diagnostics attribution and feedback routing all key on what kind of source
// this is; a source that lies about being a paired controller would answer all
// four questions wrongly and would be a trap for the next maintainer.
//
// PLATFORM SCOPE
//
// Android keeps BR/EDR HID and must not be migrated here. Carrier architecture
// differs by platform; normalized semantics stay shared.
#ifndef NS2_COMPANION_LINK_H
#define NS2_COMPANION_LINK_H

#include <stdbool.h>
#include <stdint.h>

// The normalized state structure is shared on purpose: Path C produces the same
// input_event_t every other source does, and differs only in how the bytes
// arrived.
#include "core/input_event.h"

// ============================================================================
// WIRE CONTRACT
// ============================================================================

// Data-plane version. Bump when the frame layout changes in a way an older peer
// would misread. Reported through the management control plane so a mismatch is
// refused at Start rather than discovered as garbage input.
#define NS2_COMPANION_LINK_VERSION 1u

// Windows -> adapter.
#define NS2_COMPANION_LINK_OP_STATE 0x01u

// Adapter -> Windows.
#define NS2_COMPANION_LINK_OP_OUTPUT 0x81u

// Input frame:
//   [0]    version
//   [1]    opcode
//   [2..3] sequence, little-endian, free to wrap
//   [4..]  canonical v2 report payload, report ID excluded
//
// The payload sizes below are NOT independent numbers. ns2_companion_link.c
// static-asserts every one of them against the canonical contract in
// tools/fixtures/android_controller_hid.h, and the host test decodes the shared
// tools/fixtures/bridge_report_goldens.csv -- the same fixture the Kotlin and
// C# encoders are pinned to -- so a byte-exact chain exists from the Windows
// encoder to this decoder. Do not hand-maintain these against a comment.
#define NS2_COMPANION_LINK_HEADER_BYTES 4u
#define NS2_COMPANION_LINK_PAYLOAD_BYTES 26u
#define NS2_COMPANION_LINK_FRAME_BYTES \
    (NS2_COMPANION_LINK_HEADER_BYTES + NS2_COMPANION_LINK_PAYLOAD_BYTES)

// The canonical report as its decoder wants it: report ID followed by payload.
#define NS2_COMPANION_LINK_REPORT_ID 0x01u
#define NS2_COMPANION_LINK_REPORT_BYTES (NS2_COMPANION_LINK_PAYLOAD_BYTES + 1u)

// One gameplay frame must fit ONE ATT operation:
//     4-byte Path C header + 26-byte canonical payload = 30-byte value
//     ATT Write Command overhead                       =  3 bytes
//     minimum ATT MTU                                  = 33
// Below this every report fragments, which would push gameplay onto the very
// command path this carrier exists to stay off. The runtime MEASURES the
// negotiated MTU and refuses to start rather than assuming it.
#define NS2_COMPANION_LINK_MIN_ATT_MTU (NS2_COMPANION_LINK_FRAME_BYTES + 3u)

// Output frame:
//   [0]   version
//   [1]   opcode
//   [2]   report id
//   [3..] canonical feedback payload (rumble L, rumble R, player LED, flags)
#define NS2_COMPANION_LINK_OUT_HEADER_BYTES 3u
// Derived from the canonical output contract, not chosen. The bridge output
// report is exactly four body bytes in every supported framing -- C
// ANDROID_CONTROLLER_OUTPUT_PAYLOAD_LEN, ANDROID_BRIDGE_FEEDBACK_MAX_LEN, and
// C# BridgeOutputCodec.BodySize all agree, and ns2_companion_link.c asserts it.
#define NS2_COMPANION_LINK_OUT_MAX_PAYLOAD 4u
#define NS2_COMPANION_LINK_OUT_FRAME_BYTES \
    (NS2_COMPANION_LINK_OUT_HEADER_BYTES + NS2_COMPANION_LINK_OUT_MAX_PAYLOAD)

// ============================================================================
// PURE FRAME LOGIC (host-testable, no BTstack, no hardware)
// ============================================================================

typedef enum {
    NS2_COMPANION_FRAME_OK = 0,
    NS2_COMPANION_FRAME_SHORT,     // fewer bytes than a complete frame
    NS2_COMPANION_FRAME_VERSION,   // data-plane version this build cannot read
    NS2_COMPANION_FRAME_OPCODE,    // well-formed but not an input state frame
    NS2_COMPANION_FRAME_STALE,     // superseded by a frame already applied
} ns2_companion_frame_result_t;

// Validate one inbound frame and locate its payload.
//
// The sequence field is a defensive protocol property, not a claim about ATT.
// Nothing here asserts that BLE reorders write commands on this path. What it
// does guarantee is that a stale, duplicated or out-of-order PRODUCER frame can
// never overwrite newer state: the Windows scheduler, a retry, or a future
// carrier change are all free to deliver one, and "latest state wins" has to
// survive that without depending on the transport's ordering promises.
//
// Comparison is a signed 16-bit delta so wrap is not mistaken for a rewind.
// `last_sequence`/`have_last` advance only when the frame is accepted, so a
// rejected frame cannot make the next good one look stale. `payload_out`
// receives a pointer into `data`; nothing is copied.
ns2_companion_frame_result_t ns2_companion_link_parse(const uint8_t *data,
                                                      uint16_t len,
                                                      uint16_t *last_sequence,
                                                      bool *have_last,
                                                      const uint8_t **payload_out);

// Build an outbound feedback frame. Returns the total frame length, or 0 when
// the payload does not fit -- refusing rather than truncating, so the companion
// can never decode a short frame as a complete but wrong feedback report.
uint16_t ns2_companion_link_encode_output(uint8_t report_id,
                                          const uint8_t *payload,
                                          uint8_t payload_len,
                                          uint8_t *out,
                                          uint16_t out_capacity);

// Does the negotiated ATT MTU carry one whole gameplay frame?
static inline bool ns2_companion_link_mtu_sufficient(uint16_t att_mtu) {
    return att_mtu >= NS2_COMPANION_LINK_MIN_ATT_MTU;
}

// ============================================================================
// CANONICAL REPORT DECODE
// ============================================================================

// Decode the v1 half of a canonical payload -- sticks, triggers, hat and
// buttons -- into an already-initialised event.
//
// The button map is a PARAMETER, not a table of our own. The runtime passes
// gamepad_quirks_android_bridge()->button_map, the same usage-number table the
// Classic bridge path uses, so usage-to-destination mapping exists once. This
// function owns only the fixed canonical byte layout, which the descriptor
// parser derives dynamically on the Classic path and which is constant here
// because Path C negotiates no descriptor.
//
// Hat and button semantics mirror bthid_gamepad.c exactly: hat 8 = released
// with 0 = North clockwise, usage number = bit index + 1.
//
// Motion, battery, flags and the timestamp are deliberately NOT decoded here.
// Those offsets belong to the vendor extension and are already owned by
// android_bridge_extract(); the runtime calls it with the canonical ext so
// there is one extractor, not two.
void ns2_companion_link_decode_base(const uint8_t *payload,
                                    const uint32_t *button_map,
                                    uint8_t button_map_size,
                                    input_event_t *event);

// ============================================================================
// STALE-INPUT WATCHDOG
// ============================================================================

// A Windows process that is killed mid-input stops writing without telling
// anyone. Nothing else in the system would notice: the management ACL stays up,
// so ordinary carrier loss never fires, and the last report would be held
// forever -- a stuck stick or a held button on the console.
//
// Keyed on DATA FRAMES specifically, not on management traffic: the companion
// may legitimately go quiet on the command plane while streaming, and may keep
// the command plane alive while its input thread is dead. Only the absence of
// gameplay frames means gameplay has stopped.
#define NS2_COMPANION_LINK_STALE_MS 300u

// True when the source is publishing but no frame has arrived for `stale_ms`,
// so the runtime must publish neutral state. Unsigned arithmetic, so a run-loop
// clock wrap cannot defer neutralization.
bool ns2_companion_link_input_stale(bool active,
                                    bool neutralized,
                                    uint32_t last_frame_ms,
                                    uint32_t now_ms,
                                    uint32_t stale_ms);

#endif  // NS2_COMPANION_LINK_H
