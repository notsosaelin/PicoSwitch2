// KB/M read surface. See include/ns2_kbm_commands.h for the cursor contract and
// why page indices were removed.

#include "ns2_kbm_commands.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ns2_kbm_status.h"

// Longest wrapper this file can emit, plus one worst-case row, must fit the
// wire limit -- otherwise a walk could make no progress and a client would be
// right to give up. Checked exactly (not estimated) by
// test_ns2_kbm_commands.c::test_one_item_always_fits, which serializes the
// genuinely widest row the identifier tables can produce.
//
// Rows are appended through this helper so the budget rule exists once. A row
// that does not fit is DEFERRED, never truncated: the caller stops and reports
// the cursor it reached.
typedef struct {
    char *out;
    size_t capacity;  // total bytes available for the JSON body, excl. NUL
    size_t suffix;    // bytes the closing wrapper still needs
    int used;
    bool full;
} kbm_writer_t;

static void writer_init(kbm_writer_t *w, char *out, size_t capacity,
                        size_t suffix) {
    w->out = out;
    w->capacity = capacity;
    w->suffix = suffix;
    w->used = 0;
    w->full = false;
}

// Append a fixed prefix/wrapper fragment. Unlike a row this may not be deferred,
// so a failure here means the caller's buffer cannot hold the reply at all.
static bool writer_head(kbm_writer_t *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool writer_head(kbm_writer_t *w, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(w->out + w->used, w->capacity - (size_t)w->used,
                            fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= w->capacity - (size_t)w->used) {
        w->full = true;
        return false;
    }
    w->used += written;
    return true;
}

// Append one list row if it fits with the closing wrapper still reserved.
// Returns false when the row was deferred, which is the signal to stop the walk
// -- NOT an error.
static bool writer_row(kbm_writer_t *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool writer_row(kbm_writer_t *w, const char *fmt, ...) {
    if (w->full) return false;
    // Reserve the suffix BEFORE writing, against the same budget the row is
    // written into. The historical bug in the peers inventory was to check the
    // reserve against the space left by the previous row and then write against
    // full capacity, so the reserve reserved nothing.
    size_t limit = w->capacity - w->suffix;
    if ((size_t)w->used >= limit) {
        w->full = true;
        return false;
    }
    size_t room = limit - (size_t)w->used;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(w->out + w->used, room + 1u, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written > room) {
        // vsnprintf already NUL-terminated at the limit; rewind so a deferred
        // row leaves no partial JSON behind.
        w->out[w->used] = '\0';
        w->full = true;
        return false;
    }
    w->used += written;
    return true;
}

// `next` is null exactly when the walk is complete. Emitting the sentinel as a
// number instead would make "finished" indistinguishable from "resume at 65535".
static int writer_finish(kbm_writer_t *w, const char *tail_fmt, uint16_t next) {
    int written;
    if (next == NS2_KBM_CURSOR_END) {
        written = snprintf(w->out + w->used, w->capacity - (size_t)w->used,
                           tail_fmt, "null");
    } else {
        char value[8];
        snprintf(value, sizeof(value), "%u", (unsigned)next);
        written = snprintf(w->out + w->used, w->capacity - (size_t)w->used,
                           tail_fmt, value);
    }
    if (written < 0 || (size_t)written >= w->capacity - (size_t)w->used) {
        return -1;
    }
    return w->used + written;
}

// ---------------------------------------------------------------------------
// Mapping pages
// ---------------------------------------------------------------------------

uint16_t ns2_kbm_map_item_count(const ns2_kbm_content_t *content,
                                ns2_kbm_layout_t layout) {
    static ns2_kbm_effective_t effective[NS2_KBM_MAX_EFFECTIVE];
    return ns2_kbm_effective_bindings(content, layout, effective,
                                      NS2_KBM_MAX_EFFECTIVE);
}

int ns2_kbm_format_map(const ns2_kbm_content_t *content,
                       ns2_kbm_layout_t layout, uint8_t profile_id,
                       uint16_t cursor, char *out, size_t capacity) {
    if (!content || !out || capacity == 0 || layout >= NS2_KBM_LAYOUT_COUNT) {
        return -1;
    }

    // Static because NS2_KBM_MAX_EFFECTIVE entries is more than the Pico W's
    // 2048-byte core-1 stack can hold, and this runs on the command path.
    // Single trusted management session, so there is no concurrent walk.
    static ns2_kbm_effective_t effective[NS2_KBM_MAX_EFFECTIVE];
    uint16_t total = ns2_kbm_effective_bindings(content, layout, effective,
                                                NS2_KBM_MAX_EFFECTIVE);

    kbm_writer_t w;
    // Suffix: `],"next":65535}` -- reserved at its longest, so a numeric `next`
    // can never be the thing that overflows.
    writer_init(&w, out, capacity, sizeof("],\"next\":65535}") - 1u);

    // `profile` stays the LAYOUT name because that is what it has always meant
    // here; `profileId` is the unambiguous identity of what produced the rows.
    if (!writer_head(&w,
                     "{\"profile\":\"%s\",\"profileId\":%u,\"cursor\":%u,"
                     "\"total\":%u,\"bindings\":[",
                     ns2_kbm_layout_name(layout), (unsigned)profile_id,
                     (unsigned)cursor, (unsigned)total)) {
        return -1;
    }

    uint16_t i = cursor;
    for (; i < total; ++i) {
        char source[12];
        ns2_kbm_source_format(effective[i].source, source, sizeof(source));
        if (!writer_row(&w, "%s{\"src\":\"%s\",\"dst\":\"%s\",\"custom\":%s}",
                        i > cursor ? "," : "", source,
                        ns2_kbm_destination_name(effective[i].destination),
                        effective[i].overridden ? "true" : "false")) {
            break;
        }
    }

    // A cursor at or past the end is a complete, empty page rather than an
    // error: it is what a client sees if the mapping shrank under it, and
    // answering `next:null` lets the walk terminate instead of spinning.
    uint16_t next = i < total ? i : NS2_KBM_CURSOR_END;
    // Zero rows emitted while items remain would leave the client unable to
    // progress. The buffer is too small for this content; say so rather than
    // returning a reply that cannot be walked.
    if (next != NS2_KBM_CURSOR_END && i == cursor) return -1;
    return writer_finish(&w, "],\"next\":%s}", next);
}

// ---------------------------------------------------------------------------
// Profile library
// ---------------------------------------------------------------------------

uint16_t ns2_kbm_profile_item_count(const ns2_kbm_config_t *config) {
    if (!config) return 0;
    uint16_t count = 0;
    for (uint8_t i = 0; i < NS2_KBM_MAX_PROFILES; ++i)
        if (config->profiles[i].used) count++;
    return count;
}

int ns2_kbm_format_profiles(const ns2_kbm_config_t *config, uint16_t cursor,
                            char *out, size_t capacity) {
    if (!config || !out || capacity == 0) return -1;

    // Used slots, in slot order. The order is stable across a walk because the
    // management session is serialized -- a save cannot interleave between two
    // pages of one client's walk.
    uint8_t live[NS2_KBM_MAX_PROFILES];
    uint16_t total = 0;
    for (uint8_t i = 0; i < NS2_KBM_MAX_PROFILES; ++i)
        if (config->profiles[i].used) live[total++] = i;

    kbm_writer_t w;
    writer_init(&w, out, capacity, sizeof("],\"next\":65535}") - 1u);
    if (!writer_head(&w,
                     "{\"cursor\":%u,\"total\":%u,\"max\":%u,\"profiles\":[",
                     (unsigned)cursor, (unsigned)total,
                     (unsigned)NS2_KBM_MAX_PROFILES)) {
        return -1;
    }

    uint16_t i = cursor;
    for (; i < total; ++i) {
        const ns2_kbm_profile_slot_t *slot = &config->profiles[live[i]];
        ns2_kbm_layout_t layout = (ns2_kbm_layout_t)slot->layout;
        if (!writer_row(&w,
                        "%s{\"id\":%u,\"position\":%u,\"layout\":\"%s\","
                        "\"name\":\"%.*s\",\"revision\":%u,\"overrides\":%u,"
                        "\"fingerprint\":%lu}",
                        i > cursor ? "," : "", (unsigned)slot->profile_id,
                        (unsigned)slot->position,
                        ns2_kbm_layout_name(layout),
                        (int)(NS2_KBM_PROFILE_NAME_MAX - 1u), slot->name,
                        (unsigned)slot->revision,
                        (unsigned)slot->content.overrides.count,
                        (unsigned long)ns2_kbm_content_fingerprint(
                            &slot->content, layout))) {
            break;
        }
    }

    uint16_t next = i < total ? i : NS2_KBM_CURSOR_END;
    if (next != NS2_KBM_CURSOR_END && i == cursor) return -1;
    return writer_finish(&w, "],\"next\":%s}", next);
}

// ---------------------------------------------------------------------------
// Realized mappings
// ---------------------------------------------------------------------------

// Which POSITION a layout's realized snapshot came from, derived rather than
// stored: the snapshot already names its source profile, and the profile knows
// its position. Deriving it keeps one source of truth.
static uint8_t runtime_position(const ns2_kbm_config_t *config,
                                ns2_kbm_layout_t layout) {
    uint8_t id = config->active[layout].source_id;
    if (id == NS2_KBM_PROFILE_ID_DEFAULT) return NS2_KBM_POSITION_DEFAULT;
    const ns2_kbm_profile_slot_t *slot = ns2_kbm_profile_find(config, id);
    return slot ? slot->position : (uint8_t)NS2_KBM_POSITION_DEFAULT;
}

int ns2_kbm_format_active(const ns2_kbm_config_t *config, char *out,
                          size_t capacity) {
    if (!config || !out || capacity == 0) return -1;
    kbm_writer_t w;
    writer_init(&w, out, capacity, sizeof("]}") - 1u);
    if (!writer_head(&w, "{\"active\":[")) return -1;

    for (uint8_t i = 0; i < NS2_KBM_LAYOUT_COUNT; ++i) {
        ns2_kbm_layout_t layout = (ns2_kbm_layout_t)i;
        const ns2_kbm_active_t *active = &config->active[i];
        // `sourceId` is what is RUNNING; `bootId` is what the next power-up will
        // run. They differ whenever a profile-switch key has been pressed, and a
        // client that assumed the persisted choice was the live one would report
        // the wrong profile as active for the rest of the session.
        //
        // Bounded by NS2_KBM_LAYOUT_COUNT, so a deferred row here is a buffer
        // that cannot hold the reply -- not a page boundary.
        if (!writer_row(&w,
                        "%s{\"layout\":\"%s\",\"sourceId\":%u,"
                        "\"bootPosition\":%u,\"runtimePosition\":%u,"
                        "\"revision\":%u,\"fingerprint\":%lu,"
                        "\"matchesSaved\":%s}",
                        i ? "," : "", ns2_kbm_layout_name(layout),
                        (unsigned)active->source_id,
                        (unsigned)config->boot_position[i],
                        (unsigned)runtime_position(config, layout),
                        (unsigned)active->source_revision,
                        (unsigned long)ns2_kbm_content_fingerprint(
                            &active->content, layout),
                        ns2_kbm_active_matches_source(config, layout)
                            ? "true" : "false")) {
            return -1;
        }
    }

    int written = snprintf(w.out + w.used, capacity - (size_t)w.used, "]}");
    if (written < 0 || (size_t)written >= capacity - (size_t)w.used) return -1;
    return w.used + written;
}

// ---------------------------------------------------------------------------
// Profile-switch bindings
// ---------------------------------------------------------------------------

int ns2_kbm_format_switches(const ns2_kbm_config_t *config, char *out,
                            size_t capacity) {
    if (!config || !out || capacity == 0) return -1;

    kbm_writer_t w;
    writer_init(&w, out, capacity, sizeof("],\"positions\":3}") - 1u);
    if (!writer_head(&w, "{\"switches\":[")) return -1;

    // ONE table for both layouts: a binding names a semantic POSITION, and the
    // layout is applied when the key is pressed. Bounded by four rows of ~35
    // bytes, so deliberately not paginated; the host test pins the worst case.
    bool first = true;
    for (uint8_t i = 0; i < NS2_KBM_SWITCH_BINDINGS_MAX; ++i) {
        const ns2_kbm_switch_binding_t *entry = &config->switches[i];
        if (!entry->used) continue;
        char source[12];
        ns2_kbm_source_t src = {entry->kind, entry->code};
        ns2_kbm_source_format(src, source, sizeof(source));
        if (!writer_row(&w, "%s{\"src\":\"%s\",\"position\":%u}",
                        first ? "" : ",", source, (unsigned)entry->position)) {
            return -1;
        }
        first = false;
    }

    int written = snprintf(w.out + w.used, capacity - (size_t)w.used,
                           "],\"positions\":%u}",
                           (unsigned)NS2_KBM_POSITIONS_PER_LAYOUT);
    if (written < 0 || (size_t)written >= capacity - (size_t)w.used) return -1;
    return w.used + written;
}
