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

#include <stddef.h>

#include "ns2_kbm_runtime.h"

// Render `status` as a single JSON object into `out`. Returns the number of
// characters that WOULD have been written (snprintf semantics), so a caller can
// detect truncation. `out` is always NUL-terminated when `len > 0`.
int ns2_kbm_status_format(const ns2_kbm_runtime_status_t *status, char *out,
                          size_t len);

#endif  // _NS2_KBM_STATUS_H_
