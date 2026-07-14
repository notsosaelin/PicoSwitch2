/*
 * gcusb_core.h -- pure, host-testable logic for the gcusb PC-side USB protocol lab
 * (tools/gcusb). Zero Windows-API dependency, by design, mirroring this project's own
 * established pure/impure split (src/switch_gc/switch_gc_encode.c, switch_gc_report_select.c,
 * hid_out_normalize.c, ns2_pairing_crypto.c) -- everything that can be reasoned about and tested
 * without a Windows box or real hardware lives here. The Windows-specific SetupAPI/WinUSB/HID
 * plumbing lives in gcusb_win.c and calls into this file for every safety-critical decision:
 * command allowlisting, bcdDevice target matching, rumble amplitude/duration clamping, NDJSON
 * line formatting, and replay-script parsing/validation.
 *
 * Build/test (no Windows headers, no hardware):
 *   gcc -I tools/gcusb -o test_gcusb_core tools/test_gcusb_core.c tools/gcusb/gcusb_core.c
 *   ./test_gcusb_core
 */
#ifndef GCUSB_CORE_H
#define GCUSB_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------------
 * Device identity / target safety
 *
 * Both the Pico (NSO GameCube personality) and the genuine NSO GameCube Controller enumerate as
 * VID:PID 057E:2073 -- selecting by VID/PID alone is unsafe (PROMPT.md's explicit safety
 * requirement). bcdDevice is the one field known to differ:
 *   genuine controller: 0x0101 (captured directly, see docs/switch2-gc/protocol.md)
 *   Pico (NSO GameCube): 0x0111 (deliberately NOT the real captured 0x0101 -- see
 *     src/switch_gc/switch_gc.c's device descriptor comment for why: avoiding a Windows
 *     WinUSB-driver-cache collision with the genuine unit was a real, previously-hardware-hit bug)
 * --------------------------------------------------------------------------------------------- */

#define GCUSB_BCDDEVICE_GENUINE 0x0101u
#define GCUSB_BCDDEVICE_PICO    0x0111u

typedef enum {
    GCUSB_TARGET_UNSPECIFIED = 0,
    GCUSB_TARGET_PICO,
    GCUSB_TARGET_GENUINE,
} gcusb_target_t;

/* Parses "--target pico" / "--target genuine" (case-insensitive). Returns
 * GCUSB_TARGET_UNSPECIFIED for anything else -- caller must treat that as a hard error, never a
 * silent default, per PROMPT.md ("Never fall back silently to 'first matching VID/PID'"). */
gcusb_target_t gcusb_parse_target(const char *arg);

/* Returns the single bcdDevice value a given target is REQUIRED to match. */
uint16_t gcusb_expected_bcddevice(gcusb_target_t target);

/* Core safety gate: does this target's expectation match what was actually read from the
 * device? Must be called (and must return true) before ANY write/mutating operation proceeds. */
bool gcusb_bcddevice_matches(gcusb_target_t target, uint16_t actual_bcddevice);

/* Extracts bcdDevice from a Windows USB hardware-ID string of the form
 * "USB\\VID_057E&PID_2073&REV_0111" (the REV_xxxx field is the bcdDevice in BCD form, printed
 * with leading zeros, 4 hex digits). Returns false if the string doesn't contain a REV_ field of
 * the expected shape. Pure string parsing -- no registry/SetupAPI calls, so this is testable
 * directly against a handful of known-shape strings without any hardware. */
bool gcusb_parse_bcddevice_from_hwid(const char *hwid, uint16_t *out_bcddevice);

/* ---------------------------------------------------------------------------------------------
 * Vendor-bulk command allowlist
 *
 * Byte layout (Confirmed, src/switch_gc/switch_gc.c's switch_gc_vendor_dispatch(),
 * docs/switch2-gc/protocol.md "USB init command sequence"): request bytes
 * [0]=id [1]=dir(0x91) [2]=transport(0x00) [3]=sub [4..5]=len(LE) [6..7]=00 00 [8..]=payload.
 * `id`==c[0], `sub`==c[3] are what the firmware's own dispatcher switches on, so the allowlist is
 * defined in exactly those terms.
 * --------------------------------------------------------------------------------------------- */

typedef enum {
    GCUSB_CMD_REJECTED = 0,     /* not in the allowlist at all -- default: refuse */
    GCUSB_CMD_ALLOWED,          /* safe by default (enumeration/init/report-select/reads/status) */
    GCUSB_CMD_REQUIRES_CONSOLE_CAPTURE_PROFILE,  /* pairing-shaped (0x15) -- real crypto state,
                                                     gated behind --profile console-capture, never
                                                     the bare default allowlist */
} gcusb_cmd_class_t;

/* `cmd` must point at a >=8-byte buffer already validated to be at least 8 bytes long by the
 * caller (matches switch_gc_vendor_dispatch()'s own `if (n < 8) return;` gate) -- this function
 * does not itself re-check length beyond what it needs to read id/sub/payload bytes it inspects. */
gcusb_cmd_class_t gcusb_classify_command(const uint8_t *cmd, uint32_t len);

/* Human-readable name for logging, e.g. "0x03/0x0A Select Input Report". Never NULL; returns
 * "unknown" for anything not in the table (still shown to the user, just unnamed). */
const char *gcusb_command_name(const uint8_t *cmd, uint32_t len);

/* ---------------------------------------------------------------------------------------------
 * Rumble safety
 *
 * PROMPT.md's rumble-lab requirements, distilled into pure, testable clamp functions: never
 * start at 0xFF/full-range, cap duration independent of host refresh, always resolvable to an
 * explicit stop. These do not talk to hardware -- they just compute safe bounds; gcusb_win.c is
 * responsible for actually enforcing them at every call site (rumble/rumble-sweep/stop-rumble).
 * --------------------------------------------------------------------------------------------- */

#define GCUSB_RUMBLE_MAX_FIRST_AMPLITUDE 0x40u   /* ~25% -- "low amplitude first", never 0xFF */
#define GCUSB_RUMBLE_MAX_PULSE_MS        300u    /* single pulse hard cap, independent of refresh */
#define GCUSB_RUMBLE_SWEEP_MAX_STEP_MS   150u    /* per-step cap during a bounded sweep */

/* Clamps a requested amplitude to a safe first-use ceiling unless `allow_unsafe` is set (the
 * tool's own explicit --unsafe flag, distinct from --confirm-genuine-motor, which only bypasses
 * the "you're about to drive real hardware" prompt, not the amplitude ceiling itself). */
uint8_t gcusb_clamp_rumble_amplitude(uint8_t requested, bool allow_unsafe);

/* Clamps a requested pulse duration to GCUSB_RUMBLE_MAX_PULSE_MS. Every rumble pulse must be
 * capped this way independent of whatever refresh cadence the host is otherwise using --
 * see docs/experiments/gcusb-rumble-lab-2026-07-14.md for why relying on refresh alone is what
 * produced the original bug this whole tool exists to isolate. */
uint32_t gcusb_clamp_rumble_duration_ms(uint32_t requested_ms, bool allow_unsafe);

/* Builds the 4-byte report-0x03 rumble-data field. Revised 2026-07-14 (Strong evidence, sourced
 * from the real Linux kernel "HID: nintendo" driver -- see gc_rumble_state_t's comment in
 * src/switch_gc/switch_gc.c and docs/experiments/refuted-hypotheses.md): the GameCube controller
 * has no continuous-amplitude rumble hardware at all -- data[1] is a 3-value state enum
 * (OFF=0/ON=1/STOP=2), not an amplitude byte, and data[0] is an unrelated sequence/command byte,
 * not amplitude either. `requested` here is `0` for off, nonzero for on -- this function does NOT
 * attempt genuine delta-sigma/duty-cycle amplitude simulation (what a real host does to fake
 * intermediate intensities on this hardware); it only emits a single ON or OFF state per call.
 * Simulating a duty cycle would mean calling this repeatedly at varying on/off ratios from
 * gcusb_win.c, not something this single pure builder can express -- not implemented, flagged as
 * a real gap if finer-grained amplitude testing is needed later. out must be a 4-byte buffer. */
void gcusb_build_rumble_data(uint8_t requested, uint8_t out[4]);

/* The unconditional "make it stop" payload -- all-zero 4-byte data field, the best-evidence
 * report-0x03 stop per docs/experiments/nso-gc-usb-capture-decode-2026-07-13.md's ZLP/all-zero
 * findings. A ZLP (zero-length OUT transfer) is the OTHER candidate stop mechanism and is not
 * representable as data bytes -- gcusb_win.c's stop-rumble path must try both, independently,
 * per PROMPT.md's explicit requirement. */
void gcusb_build_rumble_stop_data(uint8_t out[4]);

/* ---------------------------------------------------------------------------------------------
 * NDJSON transport logging
 * --------------------------------------------------------------------------------------------- */

typedef struct {
    uint64_t ts_us;              /* monotonic microseconds */
    const char *target;          /* "pico" / "genuine" */
    const char *iface;           /* "EP0" / "vendor-bulk" / "hid-out" / "hid-in" */
    const char *xfer_type;       /* "control" / "bulk" / "interrupt" */
    const char *direction;       /* "out" / "in" */
    const char *setup_hex;       /* control-transfer setup packet, hex string, or NULL */
    const char *req_hex;         /* exact requested bytes, hex string, or NULL */
    const char *resp_hex;        /* exact response bytes, hex string, or NULL */
    const char *status;          /* "ok" / "error:<win32 code>" / "timeout" etc. */
    uint32_t elapsed_us;
} gcusb_log_event_t;

/* Formats one NDJSON line (no trailing newline) into `out` (caller-supplied buffer of
 * `out_cap` bytes). Returns the number of bytes written (excluding the NUL terminator), or 0 if
 * `out_cap` was too small -- never writes a truncated/invalid JSON line, so a caller can tell
 * "didn't fit" apart from "wrote something" unambiguously. All string fields are JSON-escaped;
 * NULL string fields are emitted as JSON null, not the literal text "(null)". */
size_t gcusb_format_ndjson_line(const gcusb_log_event_t *ev, char *out, size_t out_cap);

/* ---------------------------------------------------------------------------------------------
 * Replay script parsing (hardware-free validation)
 * --------------------------------------------------------------------------------------------- */

typedef enum {
    GCUSB_SCRIPT_LINE_BLANK = 0,   /* blank or comment ("#...") -- nothing to do */
    GCUSB_SCRIPT_LINE_SEND,        /* "SEND <hex bytes>" */
    GCUSB_SCRIPT_LINE_SLEEP,       /* "SLEEP <ms>" */
    GCUSB_SCRIPT_LINE_STOP_RUMBLE, /* "STOP-RUMBLE" */
    GCUSB_SCRIPT_LINE_INVALID,     /* unrecognized syntax */
} gcusb_script_line_kind_t;

typedef struct {
    gcusb_script_line_kind_t kind;
    uint8_t bytes[64];       /* SEND payload, decoded from hex */
    uint32_t byte_count;
    uint32_t sleep_ms;       /* SLEEP arg */
    bool allowlisted;        /* for SEND: gcusb_classify_command() != GCUSB_CMD_REJECTED */
    gcusb_cmd_class_t cmd_class;
} gcusb_script_line_t;

/* Parses one line of a replay script (see docs/switch2-gc/ for the script format, or the
 * tools/gcusb/scripts directory for examples). Never touches hardware -- purely a text/hex parser,
 * so a whole script file can be validated (syntax + allowlist compliance) with zero devices
 * connected, satisfying PROMPT.md's "replay script validation without touching hardware"
 * requirement. `line` need not be NUL-terminated at exactly its content -- pass length via
 * `line_len` (a line read via fgets() including its own NUL is fine to pass through directly). */
void gcusb_parse_script_line(const char *line, size_t line_len, gcusb_script_line_t *out);

#ifdef __cplusplus
}
#endif

#endif  /* GCUSB_CORE_H */
