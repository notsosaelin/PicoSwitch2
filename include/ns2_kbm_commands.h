#ifndef _NS2_KBM_COMMANDS_H_
#define _NS2_KBM_COMMANDS_H_

// The KB/M *read* surface: mapping pages, the profile library, and the realized
// active mappings, rendered as JSON.
//
// WHY THIS FILE EXISTS
//
// These formatters used to live inside src/config.c, where nothing could reach
// them: config.c is bound to TinyUSB, BTstack and the flash driver, so it does
// not compile on the host. The pagination they implement was therefore covered
// only by client-side fixtures -- fixtures written by hand, from the same
// misunderstanding that produced the bug. A page-index bug shipped, reached
// hardware, and presented as `Adapter returned an incomplete KB/M binding list`.
//
// Everything here is pure: it takes a config snapshot and a caller-owned buffer
// and touches no hardware, so tools/test_ns2_kbm_commands.c can drive the REAL
// formatter across every layout, profile and cursor and prove the properties
// that matter (below). config.c is now only a dispatcher.
//
// CURSOR PAGINATION -- AND WHY NOT PAGE INDICES
//
// A reply must fit CONFIG_WIRELESS_RESPONSE_MAX_JSON, and rows have variable
// width (a 6- or 7-character source, a 4- to 12-character destination). So the
// number of rows that fits a reply is NOT a constant.
//
// The previous design asked for `page N` and answered with
// `first = N * PAGE_SIZE`, then stopped early when the byte budget ran out.
// Those two rules contradict: emitting 7 rows for a nominal page size of 8 and
// then answering the next request from index 8 skips index 7 entirely. Every
// page silently dropped its last row. The reply was individually well-formed
// and under the wire limit, `more` was true, progress looked fine -- and the
// reconstructed mapping was short, which the client could only report as a
// generic incompleteness.
//
// A fixed page size cannot be rescued by making it smaller: any constant is
// either unsafe for the worst-case row or wasteful for the common one, and the
// failure mode of guessing wrong is SILENT DATA LOSS.
//
// So the cursor is the index of the next logical item, and the reply says where
// to resume. The firmware, which is the only party that knows how many rows it
// actually managed to serialize, owns that number:
//
//   -> kbm map kb 0
//   <- {"profile":"kb","profileId":1,"cursor":0,"total":31,
//       "bindings":[ ... ],"next":7}
//   -> kbm map kb 7
//   <- {... ,"cursor":7,"total":31,"bindings":[ ... ],"next":null}
//
// `next` is null exactly when the mapping is complete. This is the same shape
// `bonds list v2` already uses in this repository, deliberately.
//
// GUARANTEES (each pinned by a host test)
//
//   1. Every reply is <= NS2_KBM_REPLY_MAX_BYTES, for worst-case content.
//   2. Every logical item appears exactly once across a cursor walk: no gaps at
//      a boundary, no duplicates.
//   3. Progress: a reply with `next` non-null always advanced the cursor, so a
//      client cannot loop.
//   4. Termination: `next` is null if and only if the walk is complete.
//   5. At least one item per reply whenever any remain, so (3) cannot be
//      satisfied only by luck. A single worst-case row plus the wrapper fits.

#include <stddef.h>
#include <stdint.h>

#include "ns2_kbm.h"
#include "ns2_kbm_status.h"  // NS2_KBM_REPLY_MAX_BYTES

// Sentinel for "no more items" in the `next` field. Serialized as JSON null.
#define NS2_KBM_CURSOR_END UINT16_MAX

// One page of a mapping, starting at logical item `cursor`.
//
// `content`/`layout` select what is rendered; `profile_id` is reported as
// `profileId` and is NS2_KBM_PROFILE_ID_NONE for a layout's realized mapping.
//
// Returns the number of bytes written (excluding the NUL), or -1 if `out` is
// too small to hold even the wrapper plus one row. Never truncates: a row that
// does not fit is deferred to the next cursor rather than half-written.
int ns2_kbm_format_map(const ns2_kbm_content_t *content,
                       ns2_kbm_layout_t layout, uint8_t profile_id,
                       uint16_t cursor, char *out, size_t capacity);

// One page of the profile library, starting at logical profile `cursor`.
// Only USED slots are logical items, and they are walked in slot order, which
// is stable for the lifetime of a walk because the management session is
// serialized. Reports `max` so a client can show "5 of 6 used".
int ns2_kbm_format_profiles(const ns2_kbm_config_t *config, uint16_t cursor,
                            char *out, size_t capacity);

// The realized mapping identity of every layout, in one reply. Bounded by
// NS2_KBM_LAYOUT_COUNT, so this is deliberately not paginated -- the host test
// pins its worst-case size against the wire limit.
int ns2_kbm_format_active(const ns2_kbm_config_t *config, char *out,
                          size_t capacity);

// The number of logical items each walk should yield. Exposed so a test can
// state the expected count independently of the walk it is checking, rather
// than comparing the formatter against itself.
uint16_t ns2_kbm_map_item_count(const ns2_kbm_content_t *content,
                                ns2_kbm_layout_t layout);
uint16_t ns2_kbm_profile_item_count(const ns2_kbm_config_t *config);

#endif  // _NS2_KBM_COMMANDS_H_
