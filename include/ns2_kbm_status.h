#ifndef _NS2_KBM_STATUS_H_
#define _NS2_KBM_STATUS_H_

// One authority for rendering the KB/M status snapshot as JSON.
//
// This exists because the management and UART surfaces each had their own
// printf with its own format string and argument list, and adding one field to
// the struct shifted every argument in one of them -- silently, because printf
// argument/format drift is not a compile error. The result was a diagnostic
// reporting `"override":"kb","profile":"false"` and a garbage counter, which is
// worse than no diagnostic at all: it invites wrong conclusions about hardware.
//
// Both surfaces now call this, and one host test pins the field order and types.

#include <stdbool.h>
#include <stddef.h>

#include "config_wireless_bridge.h"
#include "ns2_kbm_runtime.h"

// Every management reply must fit the wireless bridge's response slot or the
// bridge refuses it and the client sees `response_too_large` -- which fails a
// whole page read, not one field. Derived from the bridge's own constant rather
// than restating 512, so there is exactly one place where the wire limit lives.
#define NS2_KBM_REPLY_MAX_BYTES CONFIG_WIRELESS_RESPONSE_MAX_JSON

// Render `status` as a single JSON object into `out`. Returns the number of
// characters that WOULD have been written (snprintf semantics), so a caller can
// detect truncation. `out` is always NUL-terminated when `len > 0`.
//
// Product state only. The ingress counters are a separate reply because the two
// together outgrew the wire slot; see ns2_kbm_status.c.
int ns2_kbm_status_format(const ns2_kbm_runtime_status_t *status, char *out,
                          size_t len);

// The ingress counters, read on demand rather than on every refresh.
int ns2_kbm_counters_format(const ns2_kbm_runtime_status_t *status, char *out,
                            size_t len);

// ---------------------------------------------------------------------------
// Mouse-translation settings command
// ---------------------------------------------------------------------------
// Same rule as the status snapshot above, for the same reason: exactly one
// parser and one response schema, shared by the management/CDC command surface
// (src/config.c) and the UART diagnostic channel (src/ns2_uart_diag.c). A
// second copy of either would be free to drift, and a settings surface that
// disagrees with itself is worse than one that is missing.
//
// Both functions are deliberately PURE -- they take and return the settings
// struct rather than reaching for the runtime. That is what keeps range
// validation in one place (ns2_kbm_runtime_set_mouse() rejects rather than
// clamps, via ns2_kbm_config_sanitize()) and what makes the command surface
// host-testable with no firmware stubs.

// Render the mouse-translation settings, and the limits a client needs in order
// to offer them, as a single JSON object. snprintf semantics, as above.
int ns2_kbm_mouse_format(const ns2_kbm_mouse_config_t *mouse, char *out,
                         size_t len);

// Parse one "<field> <value>" setting out of `args` and apply it to `mouse`.
//
// Returns false when the text is malformed, the field is unknown, or the value
// cannot be represented -- NOT when the value is merely outside the configured
// range. Range enforcement stays with ns2_kbm_runtime_set_mouse(), so a caller
// applies this to a copy and then stores it:
//
//     ns2_kbm_runtime_get_mouse(&mouse);
//     if (!ns2_kbm_mouse_command_apply(&mouse, args)) -> usage error
//     if (!ns2_kbm_runtime_set_mouse(&mouse))         -> out of range, rejected
bool ns2_kbm_mouse_command_apply(ns2_kbm_mouse_config_t *mouse,
                                 const char *args);

#endif  // _NS2_KBM_STATUS_H_
