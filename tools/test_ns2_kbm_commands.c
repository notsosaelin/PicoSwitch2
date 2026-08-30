// The KB/M read surface: cursor pagination, wire budget, and exact
// reconstruction.
//
// WHY THIS TEST EXISTS
//
// Two bugs reached hardware in this area, and neither was catchable by the tests
// that existed:
//
//   1. `kbm status` grew past the wireless reply slot. The test asserted against
//      a 2048-byte FIRMWARE-LOCAL buffer instead of the wire limit, so it passed
//      while the adapter answered `response_too_large` and the whole Keyboard &
//      Mouse page failed to load.
//
//   2. Mapping pagination combined a fixed logical offset (`page * PAGE_SIZE`)
//      with a variable, byte-budgeted number of emitted rows. When the budget
//      cut a page short at 7 of 8 rows, the next request still resumed at index
//      8 and index 7 was lost -- silently, in every page. Each reply was
//      individually valid and under the limit; only the reconstructed total was
//      wrong, which the client could report only as `Adapter returned an
//      incomplete KB/M binding list`.
//
// Both were pagination/serialization properties of firmware code that lived in
// src/config.c, which does not compile on the host. The formatters now live in
// src/ns2_kbm_commands.c so this test can drive the REAL implementation.
//
// The properties below are the ones whose absence produced those defects. They
// are checked against worst-case content, not against a convenient sample.
//
// This test also emits tools/fixtures/management/kbm-wire-corpus.json: the exact
// bytes the real firmware produces for a full cursor walk. The Windows and
// Android integration tests replay that corpus through their real clients, so
// all three implementations are checked against one authority instead of three
// hand-written guesses at the same wire format.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ns2_kbm_commands.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// Minimal JSON field readers. Deliberately tiny: this test must not depend on a
// parser that could paper over a malformed reply.
// ---------------------------------------------------------------------------

static const char *field(const char *json, const char *key) {
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = strstr(json, needle);
    return at ? at + strlen(needle) : NULL;
}

static long number_field(const char *json, const char *key, long missing) {
    const char *at = field(json, key);
    if (!at) return missing;
    if (strncmp(at, "null", 4) == 0) return missing;
    return strtol(at, NULL, 10);
}

// Count occurrences of `"src":"` so a row count is derived from the payload
// rather than from the formatter's own bookkeeping.
static int count_rows(const char *json, const char *marker) {
    int n = 0;
    for (const char *at = json; (at = strstr(at, marker)) != NULL; at += 1) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Corpus emission
// ---------------------------------------------------------------------------
// One record per reply, with the separator owned by the FILE rather than by
// whichever walk happened to be running -- a per-walk separator produced invalid
// JSON the moment a second walk started, which is a poor way to ship a fixture
// three other test suites depend on.

static int corpus_records;

static void corpus_emit(FILE *corpus, const char *label, const char *command,
                        int bytes, const char *reply) {
    if (!corpus) return;
    fprintf(corpus, "%s    {\"label\":\"%s\",\"command\":\"%s\",\"bytes\":%d,"
                    "\"reply\":%s}",
            corpus_records++ ? ",\n" : "", label, command, bytes, reply);
}

// ---------------------------------------------------------------------------
// Content builders
// ---------------------------------------------------------------------------

// The widest mapping the model can express: every override slot filled with a
// source that has no canonical binding, so it becomes an ADDITIONAL row on top
// of the default table. This is the content that makes rows longest and the
// list longest at the same time, which is exactly where a page boundary bug
// hides.
static void build_worst_case_content(ns2_kbm_content_t *content,
                                     ns2_kbm_layout_t layout) {
    ns2_kbm_template_default(layout, content);
    // Usages well above the default tables, so each is a new binding rather
    // than a re-point of an existing one.
    uint8_t code = 0x80u;
    for (uint8_t i = 0; i < NS2_KBM_MAX_OVERRIDES; ++i) {
        ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, code++};
        // The longest destination name the identifier table can produce, so
        // every row is at its maximum serialized width.
        uint8_t destination = 0;
        (void)ns2_kbm_destination_from_name("rstick_right", &destination);
        if (!ns2_kbm_set_binding(content, layout, source, destination)) break;
    }
}

static void fill_profile_library(ns2_kbm_config_t *config) {
    for (uint8_t i = 0; i < NS2_KBM_MAX_PROFILES; ++i) {
        ns2_kbm_layout_t layout = (i % 2u) ? NS2_KBM_LAYOUT_KEYBOARD_MOUSE
                                           : NS2_KBM_LAYOUT_KEYBOARD;
        ns2_kbm_content_t content;
        build_worst_case_content(&content, layout);
        char name[NS2_KBM_PROFILE_NAME_MAX];
        // Maximum-length name: a shorter one would understate every row.
        memset(name, 'W', sizeof(name) - 1u);
        name[sizeof(name) - 1u] = '\0';
        name[0] = (char)('A' + i);  // distinct, so duplicates are detectable
        uint8_t id = ns2_kbm_profile_create(config, layout, name, &content);
        CHECK(id != NS2_KBM_PROFILE_ID_NONE, "profile %u could not be created",
              i);
        // Drive the revision high enough to be worst-case in width.
        ns2_kbm_profile_slot_t *slot =
            (ns2_kbm_profile_slot_t *)ns2_kbm_profile_find(config, id);
        if (slot) slot->revision = 65535u;
    }
}

// ---------------------------------------------------------------------------
// The walk, with the guarantees checked on every step
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t items[NS2_KBM_MAX_EFFECTIVE * 2u];
    uint16_t count;
    int replies;
    size_t largest_reply;
} walk_t;

// Walks a mapping the way a client must, and records what arrived. Returns
// false if the walk could not complete; the CHECKs report why.
static bool walk_map(const ns2_kbm_content_t *content, ns2_kbm_layout_t layout,
                     uint8_t profile_id, walk_t *walk, FILE *corpus,
                     const char *corpus_label) {
    memset(walk, 0, sizeof(*walk));
    char reply[NS2_KBM_REPLY_MAX_BYTES + 64u];
    uint16_t cursor = 0;
    uint16_t expected_total = ns2_kbm_map_item_count(content, layout);

    for (int guard = 0; guard <= (int)NS2_KBM_MAX_EFFECTIVE + 2; ++guard) {
        int len = ns2_kbm_format_map(content, layout, profile_id, cursor, reply,
                                     NS2_KBM_REPLY_MAX_BYTES + 1u);
        CHECK(len > 0, "format_map(cursor=%u) failed", cursor);
        if (len <= 0) return false;

        // (1) THE WIRE BUDGET. Not the local buffer -- the slot the bridge will
        // refuse to exceed.
        CHECK((size_t)len <= NS2_KBM_REPLY_MAX_BYTES,
              "reply at cursor %u is %d bytes, limit is %u", cursor, len,
              (unsigned)NS2_KBM_REPLY_MAX_BYTES);
        CHECK(strlen(reply) == (size_t)len, "returned length disagrees with the"
              " NUL-terminated payload at cursor %u", cursor);
        if ((size_t)len > walk->largest_reply) walk->largest_reply = (size_t)len;
        walk->replies++;

        {
            // The command a client would actually have sent for this reply,
            // cursor and all, so the fixture is replayable verbatim.
            char command[48];
            if (profile_id == NS2_KBM_PROFILE_ID_NONE) {
                snprintf(command, sizeof(command), "kbm map %s %u",
                         ns2_kbm_layout_name(layout), cursor);
            } else {
                snprintf(command, sizeof(command), "kbm pmap %u %u",
                         (unsigned)profile_id, cursor);
            }
            corpus_emit(corpus, corpus_label, command, len, reply);
        }

        // The reply must say where it started and how many items exist, so the
        // client can detect a shifting list rather than reconstructing garbage.
        CHECK(number_field(reply, "cursor", -1) == (long)cursor,
              "reply echoed cursor %ld, asked for %u",
              number_field(reply, "cursor", -1), cursor);
        CHECK(number_field(reply, "total", -1) == (long)expected_total,
              "reply says total %ld, mapping has %u",
              number_field(reply, "total", -1), expected_total);

        int rows = count_rows(reply, "\"src\":\"");
        for (int i = 0; i < rows; ++i) {
            CHECK(walk->count < (uint16_t)(sizeof(walk->items) /
                                           sizeof(walk->items[0])),
                  "walk overflowed");
            walk->items[walk->count++] = (uint16_t)(cursor + (uint16_t)i);
        }

        long next = number_field(reply, "next", -1);
        if (next < 0) {
            // (4) TERMINATION: `next:null` claims completeness, so it must be
            // true.
            CHECK((uint16_t)(cursor + rows) == expected_total,
                  "walk ended at %u of %u items", (unsigned)(cursor + rows),
                  expected_total);
            return true;
        }

        // (5) AT LEAST ONE ITEM whenever items remain.
        CHECK(rows > 0, "reply at cursor %u carried no rows but asked to"
              " continue", cursor);
        // (3) PROGRESS, and (2)'s precondition: the next cursor is exactly the
        // first item NOT emitted. Anything else drops or repeats rows.
        CHECK(next == (long)cursor + rows,
              "next is %ld but cursor %u emitted %d rows -- a gap or an overlap",
              next, cursor, rows);
        CHECK(next > (long)cursor, "cursor did not advance past %u", cursor);
        cursor = (uint16_t)next;
    }

    CHECK(false, "walk did not terminate");
    return false;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// (2) Exact reconstruction. The property the shipped bug violated: not "the
// pages looked reasonable" but "every logical item arrived exactly once".
static void test_walk_reconstructs_every_mapping_exactly_once(FILE *corpus) {
    printf("walk reconstructs every mapping exactly once\n");
    for (unsigned l = 0; l < NS2_KBM_LAYOUT_COUNT; ++l) {
        ns2_kbm_layout_t layout = (ns2_kbm_layout_t)l;
        ns2_kbm_content_t content;
        build_worst_case_content(&content, layout);

        // The expectation is computed INDEPENDENTLY of the formatter: straight
        // from the model. Comparing the walk against the formatter's own idea
        // of its total is how a self-consistent but wrong pagination passes.
        static ns2_kbm_effective_t expected[NS2_KBM_MAX_EFFECTIVE];
        uint16_t expected_count = ns2_kbm_effective_bindings(
            &content, layout, expected, NS2_KBM_MAX_EFFECTIVE);
        CHECK(expected_count > 8u,
              "%s has only %u bindings -- too few to cross a page boundary,"
              " which is the case this test exists to cover",
              ns2_kbm_layout_name(layout), expected_count);

        walk_t walk;
        char label[32];
        snprintf(label, sizeof(label), "map-%s", ns2_kbm_layout_name(layout));
        if (!walk_map(&content, layout, NS2_KBM_PROFILE_ID_NONE, &walk, corpus,
                      label)) {
            continue;
        }

        CHECK(walk.count == expected_count,
              "%s: reconstructed %u of %u bindings (%d replies)",
              ns2_kbm_layout_name(layout), walk.count, expected_count,
              walk.replies);

        // No gaps, no duplicates: the walk must have produced index i at
        // position i, for every i.
        int missing = 0, duplicated = 0;
        static uint8_t seen[NS2_KBM_MAX_EFFECTIVE * 2u];
        memset(seen, 0, sizeof(seen));
        for (uint16_t i = 0; i < walk.count; ++i) {
            if (walk.items[i] < sizeof(seen)) {
                if (seen[walk.items[i]]++) duplicated++;
            }
        }
        for (uint16_t i = 0; i < expected_count; ++i)
            if (!seen[i]) missing++;
        CHECK(missing == 0, "%s: %d source(s) missing from the walk",
              ns2_kbm_layout_name(layout), missing);
        CHECK(duplicated == 0, "%s: %d duplicate source(s) in the walk",
              ns2_kbm_layout_name(layout), duplicated);

        // More than one reply, or the pagination is not being exercised at all.
        CHECK(walk.replies > 1,
              "%s fit in one reply (%zu bytes); this test is not covering a"
              " boundary", ns2_kbm_layout_name(layout), walk.largest_reply);
        printf("  %s: %u bindings in %d replies, largest %zu/%u bytes\n",
               ns2_kbm_layout_name(layout), walk.count, walk.replies,
               walk.largest_reply, (unsigned)NS2_KBM_REPLY_MAX_BYTES);
    }
}

// The specific regression. Reproduces what the fixed-page-size formatter did and
// asserts the current one cannot behave that way: if a reply stops short of a
// nominal page size, the next cursor MUST be where it actually stopped.
static void test_a_short_page_resumes_where_it_stopped(void) {
    printf("a short page resumes where it stopped\n");
    ns2_kbm_content_t content;
    build_worst_case_content(&content, NS2_KBM_LAYOUT_KEYBOARD_MOUSE);

    char reply[NS2_KBM_REPLY_MAX_BYTES + 64u];
    int len = ns2_kbm_format_map(&content, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                                 NS2_KBM_PROFILE_ID_NONE, 0, reply,
                                 NS2_KBM_REPLY_MAX_BYTES + 1u);
    CHECK(len > 0, "format failed");
    int rows = count_rows(reply, "\"src\":\"");
    long next = number_field(reply, "next", -1);

    // The old formatter answered `first + KBM_MAP_PAGE_SIZE` (8) here while
    // emitting 7 rows, losing index 7 on every page.
    CHECK(next == rows, "next=%ld after %d rows: the old formatter answered a"
          " fixed page stride and dropped the difference", next, rows);
    CHECK(rows < 8 || next == rows,
          "a full page must still resume at the row after the last emitted");

    // And the item at that cursor must be the one that was deferred, not the
    // one after it: walk two replies and check the join.
    char second[NS2_KBM_REPLY_MAX_BYTES + 64u];
    int len2 = ns2_kbm_format_map(&content, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                                  NS2_KBM_PROFILE_ID_NONE, (uint16_t)next,
                                  second, NS2_KBM_REPLY_MAX_BYTES + 1u);
    CHECK(len2 > 0, "second page failed");

    static ns2_kbm_effective_t expected[NS2_KBM_MAX_EFFECTIVE];
    (void)ns2_kbm_effective_bindings(&content, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                                     expected, NS2_KBM_MAX_EFFECTIVE);
    char wanted[16];
    ns2_kbm_source_format(expected[next].source, wanted, sizeof(wanted));
    char needle[32];
    snprintf(needle, sizeof(needle), "\"src\":\"%s\"", wanted);
    CHECK(strstr(second, needle) != NULL,
          "the row at cursor %ld (%s) is absent from the next reply -- it was"
          " skipped", next, wanted);
}

// (5) One worst-case row plus the wrapper must fit, or a walk could stall
// forever on content the user is allowed to create.
static void test_one_item_always_fits(void) {
    printf("one worst-case item always fits a reply\n");
    ns2_kbm_content_t content;
    build_worst_case_content(&content, NS2_KBM_LAYOUT_KEYBOARD_MOUSE);
    uint16_t total = ns2_kbm_map_item_count(&content,
                                            NS2_KBM_LAYOUT_KEYBOARD_MOUSE);
    // From every cursor, including the last, a reply must carry >= 1 row.
    for (uint16_t cursor = 0; cursor < total; ++cursor) {
        char reply[NS2_KBM_REPLY_MAX_BYTES + 64u];
        int len = ns2_kbm_format_map(&content, NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                                     NS2_KBM_PROFILE_ID_MAX, cursor, reply,
                                     NS2_KBM_REPLY_MAX_BYTES + 1u);
        CHECK(len > 0 && (size_t)len <= NS2_KBM_REPLY_MAX_BYTES,
              "cursor %u produced %d bytes", cursor, len);
        CHECK(count_rows(reply, "\"src\":\"") >= 1,
              "cursor %u produced an empty reply while %u items remain", cursor,
              (unsigned)(total - cursor));
    }
}

// A cursor past the end terminates rather than spinning. This is what a client
// sees if the library shrank between two of its requests.
static void test_a_cursor_past_the_end_terminates(void) {
    printf("a cursor past the end terminates\n");
    ns2_kbm_content_t content;
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    uint16_t total = ns2_kbm_map_item_count(&content, NS2_KBM_LAYOUT_KEYBOARD);

    char reply[NS2_KBM_REPLY_MAX_BYTES + 64u];
    int len = ns2_kbm_format_map(&content, NS2_KBM_LAYOUT_KEYBOARD,
                                 NS2_KBM_PROFILE_ID_NONE,
                                 (uint16_t)(total + 5u), reply,
                                 NS2_KBM_REPLY_MAX_BYTES + 1u);
    CHECK(len > 0, "a past-the-end cursor must still answer");
    CHECK(count_rows(reply, "\"src\":\"") == 0, "expected no rows");
    CHECK(strstr(reply, "\"next\":null") != NULL,
          "a past-the-end cursor must terminate the walk: %s", reply);
}

// The profile library walks under the same contract, with a FULL library of
// maximum-length names and maximum revisions.
static void test_profile_library_walk(FILE *corpus) {
    printf("profile library walks completely under the wire budget\n");
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    fill_profile_library(&config);

    uint16_t expected = ns2_kbm_profile_item_count(&config);
    CHECK(expected == NS2_KBM_MAX_PROFILES, "expected a full library, have %u",
          expected);

    char reply[NS2_KBM_REPLY_MAX_BYTES + 64u];
    uint16_t cursor = 0;
    uint16_t seen = 0;
    int replies = 0;
    size_t largest = 0;
    for (int guard = 0; guard <= (int)NS2_KBM_MAX_PROFILES + 2; ++guard) {
        int len = ns2_kbm_format_profiles(&config, cursor, reply,
                                          NS2_KBM_REPLY_MAX_BYTES + 1u);
        CHECK(len > 0, "format_profiles(cursor=%u) failed", cursor);
        if (len <= 0) return;
        CHECK((size_t)len <= NS2_KBM_REPLY_MAX_BYTES,
              "profile reply at cursor %u is %d bytes, limit %u", cursor, len,
              (unsigned)NS2_KBM_REPLY_MAX_BYTES);
        if ((size_t)len > largest) largest = (size_t)len;
        replies++;
        {
            char command[32];
            snprintf(command, sizeof(command), "kbm profiles %u", cursor);
            corpus_emit(corpus, "profiles", command, len, reply);
        }

        CHECK(number_field(reply, "cursor", -1) == (long)cursor,
              "profile reply echoed the wrong cursor");
        CHECK(number_field(reply, "total", -1) == (long)expected,
              "profile reply reported total %ld, expected %u",
              number_field(reply, "total", -1), expected);
        CHECK(number_field(reply, "max", -1) == (long)NS2_KBM_MAX_PROFILES,
              "profile reply must report the slot count");

        int rows = count_rows(reply, "\"id\":");
        seen = (uint16_t)(seen + rows);
        long next = number_field(reply, "next", -1);
        if (next < 0) break;
        CHECK(rows > 0, "profile page carried no rows but asked to continue");
        CHECK(next == (long)cursor + rows,
              "profile next=%ld after %d rows from cursor %u", next, rows,
              cursor);
        cursor = (uint16_t)next;
    }
    CHECK(seen == expected, "reconstructed %u of %u profiles in %d replies",
          seen, expected, replies);
    printf("  %u profiles in %d replies, largest %zu/%u bytes\n", seen, replies,
           largest, (unsigned)NS2_KBM_REPLY_MAX_BYTES);
}

// `kbm active` is not paginated, so its worst case must fit outright.
static void test_active_fits_without_pagination(FILE *corpus) {
    printf("active mappings fit one reply\n");
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    fill_profile_library(&config);
    // Realize a profile into each layout so the reply carries real ids,
    // revisions and fingerprints rather than zeros.
    for (uint8_t i = 0; i < NS2_KBM_MAX_PROFILES; ++i) {
        const ns2_kbm_profile_slot_t *slot = &config.profiles[i];
        if (slot->used) {
            bool changed = false;
            (void)ns2_kbm_apply(&config, (ns2_kbm_layout_t)slot->layout,
                                slot->profile_id, &changed);
        }
    }

    char reply[NS2_KBM_REPLY_MAX_BYTES + 64u];
    int len = ns2_kbm_format_active(&config, reply,
                                    NS2_KBM_REPLY_MAX_BYTES + 1u);
    CHECK(len > 0, "format_active failed");
    CHECK((size_t)len <= NS2_KBM_REPLY_MAX_BYTES,
          "active reply is %d bytes, limit %u", len,
          (unsigned)NS2_KBM_REPLY_MAX_BYTES);
    CHECK(count_rows(reply, "\"layout\":\"") == NS2_KBM_LAYOUT_COUNT,
          "active must report every layout");
    printf("  active: %d/%u bytes\n", len, (unsigned)NS2_KBM_REPLY_MAX_BYTES);
    corpus_emit(corpus, "active", "kbm active", len, reply);
}

// `kbm switches` is not paginated, so its worst case must fit outright: every
// slot bound, longest source spelling, widest ids.
static void test_switch_list_fits_without_pagination(FILE *corpus) {
    printf("switch bindings fit one reply\n");
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    fill_profile_library(&config);

    // Every semantic action bound to a key: Default plus the three positions.
    unsigned bound = 0;
    for (uint8_t position = 0; position < NS2_KBM_SWITCH_BINDINGS_MAX;
         ++position) {
        ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, (uint8_t)(0x3Au + position)};
        if (ns2_kbm_switch_bind(&config, source, position)) bound++;
    }
    CHECK(bound == NS2_KBM_SWITCH_BINDINGS_MAX,
          "expected every action bound, got %u", bound);

    char reply[NS2_KBM_REPLY_MAX_BYTES + 64u];
    int len = ns2_kbm_format_switches(&config, reply,
                                      NS2_KBM_REPLY_MAX_BYTES + 1u);
    CHECK(len > 0, "format_switches failed");
    CHECK((size_t)len <= NS2_KBM_REPLY_MAX_BYTES,
          "switch reply is %d bytes, limit %u", len,
          (unsigned)NS2_KBM_REPLY_MAX_BYTES);
    CHECK(count_rows(reply, "\"src\":\"") == (int)bound,
          "every binding must be listed");
    printf("  switches: %d/%u bytes, %u bound\n", len,
           (unsigned)NS2_KBM_REPLY_MAX_BYTES, bound);
    corpus_emit(corpus, "switches", "kbm switches kb", len, reply);
}

// A stored profile's mapping uses the same walk as a realized one. They diverged
// once already; sharing the formatter is what keeps them honest.
static void test_stored_profile_mapping_walks_identically(FILE *corpus) {
    printf("a stored profile's mapping walks under the same contract\n");
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    fill_profile_library(&config);

    const ns2_kbm_profile_slot_t *slot = NULL;
    for (uint8_t i = 0; i < NS2_KBM_MAX_PROFILES; ++i)
        if (config.profiles[i].used) { slot = &config.profiles[i]; break; }
    CHECK(slot != NULL, "expected a stored profile");
    if (!slot) return;

    walk_t walk;
    if (!walk_map(&slot->content, (ns2_kbm_layout_t)slot->layout,
                  slot->profile_id, &walk, corpus, "pmap")) {
        return;
    }
    uint16_t expected = ns2_kbm_map_item_count(&slot->content,
                                               (ns2_kbm_layout_t)slot->layout);
    CHECK(walk.count == expected, "stored profile reconstructed %u of %u",
          walk.count, expected);
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The corpus: ONE coherent adapter, walked the way a client bootstraps a page.
// ---------------------------------------------------------------------------
// Emitted from a single config so the records agree with each other -- the
// `active` reply names profiles that are in the `profiles` reply, and the map
// walks are of the mappings those profiles realized. Fragments taken from
// several independent tests would not be a scenario a real adapter could ever
// produce, and a client integration test built on one would be checking against
// a situation that cannot happen.
static void emit_corpus(FILE *corpus) {
    printf("emitting the wire corpus for the client integration tests\n");
    ns2_kbm_config_t config;
    ns2_kbm_config_defaults(&config);
    fill_profile_library(&config);

    // Realize the first profile of each layout, so `active` reports a custom
    // source id rather than Default for both.
    for (unsigned l = 0; l < NS2_KBM_LAYOUT_COUNT; ++l) {
        for (uint8_t i = 0; i < NS2_KBM_MAX_PROFILES; ++i) {
            const ns2_kbm_profile_slot_t *slot = &config.profiles[i];
            if (slot->used && slot->layout == l) {
                bool changed = false;
                (void)ns2_kbm_apply(&config, (ns2_kbm_layout_t)l,
                                    slot->profile_id, &changed);
                break;
            }
        }
    }

    // The two realized mappings, exactly as `kbm map kb|kbm` returns them.
    for (unsigned l = 0; l < NS2_KBM_LAYOUT_COUNT; ++l) {
        ns2_kbm_layout_t layout = (ns2_kbm_layout_t)l;
        walk_t walk;
        char label[32];
        snprintf(label, sizeof(label), "map-%s", ns2_kbm_layout_name(layout));
        (void)walk_map(&config.active[l].content, layout,
                       NS2_KBM_PROFILE_ID_NONE, &walk, corpus, label);
    }

    // One stored profile's mapping, as `kbm pmap <id>` returns it.
    for (uint8_t i = 0; i < NS2_KBM_MAX_PROFILES; ++i) {
        const ns2_kbm_profile_slot_t *slot = &config.profiles[i];
        if (slot->used) {
            walk_t walk;
            (void)walk_map(&slot->content, (ns2_kbm_layout_t)slot->layout,
                           slot->profile_id, &walk, corpus, "pmap");
            break;
        }
    }

    // The library.
    char reply[NS2_KBM_REPLY_MAX_BYTES + 64u];
    uint16_t cursor = 0;
    for (int guard = 0; guard <= (int)NS2_KBM_MAX_PROFILES + 2; ++guard) {
        int len = ns2_kbm_format_profiles(&config, cursor, reply,
                                          NS2_KBM_REPLY_MAX_BYTES + 1u);
        if (len <= 0) break;
        char command[32];
        snprintf(command, sizeof(command), "kbm profiles %u", cursor);
        corpus_emit(corpus, "profiles", command, len, reply);
        long next = number_field(reply, "next", -1);
        if (next < 0) break;
        cursor = (uint16_t)next;
    }

    int len = ns2_kbm_format_active(&config, reply,
                                    NS2_KBM_REPLY_MAX_BYTES + 1u);
    if (len > 0) corpus_emit(corpus, "active", "kbm active", len, reply);

    // The four semantic switch actions, so the clients have real bytes too.
    for (uint8_t position = 0; position < NS2_KBM_SWITCH_BINDINGS_MAX;
         ++position) {
        ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, (uint8_t)(0x3Au + position)};
        (void)ns2_kbm_switch_bind(&config, source, position);
    }
    len = ns2_kbm_format_switches(&config, reply,
                                  NS2_KBM_REPLY_MAX_BYTES + 1u);
    if (len > 0) corpus_emit(corpus, "switches", "kbm switches", len, reply);
}

// ---------------------------------------------------------------------------
// Fingerprint parity vectors
// ---------------------------------------------------------------------------
// The fingerprint is what answers "is the adapter's copy of this profile still
// the one I have?" ACROSS PLATFORMS -- Windows and Android keep separate local
// libraries with separate ids, so content is the only thing they can compare.
//
// Three implementations therefore have to agree byte for byte. These vectors are
// produced by the firmware's own function, so the clients can be tested against
// the authority rather than against each other.
static void emit_one_vector(FILE *out, const char *label,
                            ns2_kbm_layout_t layout,
                            const ns2_kbm_content_t *content, bool first) {
    ns2_kbm_content_t canonical = *content;
    ns2_kbm_content_canonicalize(&canonical, layout);

    fprintf(out, "%s    {\"label\":\"%s\",\"layout\":\"%s\",\"overrides\":[",
            first ? "" : ",\n", label, ns2_kbm_layout_name(layout));
    for (uint8_t i = 0; i < canonical.overrides.count; ++i) {
        const ns2_kbm_override_t *e = &canonical.overrides.entries[i];
        char source[12];
        ns2_kbm_source_format(e->source, source, sizeof(source));
        fprintf(out, "%s{\"src\":\"%s\",\"dst\":\"%s\"}", i ? "," : "", source,
                ns2_kbm_destination_name(e->destination));
    }
    fprintf(out,
            "],\"mouse\":{\"sensitivityX\":%u,\"sensitivityY\":%u,"
            "\"velocityWindowMs\":%u,\"invertX\":%s,\"invertY\":%s,"
            "\"antiDeadzone\":%u},\"fingerprint\":%lu}",
            (unsigned)canonical.mouse.sensitivity_x,
            (unsigned)canonical.mouse.sensitivity_y,
            (unsigned)canonical.mouse.recenter_ms,
            canonical.mouse.invert_x ? "true" : "false",
            canonical.mouse.invert_y ? "true" : "false",
            (unsigned)canonical.mouse.anti_deadzone,
            (unsigned long)ns2_kbm_content_fingerprint(&canonical, layout));
}

static void emit_fingerprint_vectors(FILE *out) {
    fprintf(out, "  \"fingerprints\": [\n");

    ns2_kbm_content_t content;

    // An untouched Default: the baseline every client must reproduce.
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    emit_one_vector(out, "kb-default", NS2_KBM_LAYOUT_KEYBOARD, &content, true);

    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD_MOUSE, &content);
    emit_one_vector(out, "kbm-default", NS2_KBM_LAYOUT_KEYBOARD_MOUSE, &content,
                    false);

    // One rebind. Proves the override contributes.
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    {
        ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, 0x04u};
        (void)ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, source,
                                  NS2_DST_ZR);
    }
    emit_one_vector(out, "kb-one-rebind", NS2_KBM_LAYOUT_KEYBOARD, &content,
                    false);

    // Several rebinds inserted OUT OF ORDER, so a client that fails to sort
    // canonically produces a different digest and this catches it.
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    {
        const uint8_t codes[] = {0x20u, 0x05u, 0x1Au, 0x0Cu};
        const uint8_t dsts[] = {NS2_DST_A, NS2_DST_B, NS2_DST_X, NS2_DST_Y};
        for (unsigned i = 0; i < 4; ++i) {
            ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, codes[i]};
            (void)ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, source,
                                      dsts[i]);
        }
    }
    emit_one_vector(out, "kb-unsorted-rebinds", NS2_KBM_LAYOUT_KEYBOARD,
                    &content, false);

    // Mouse settings only. Profile-owned tuning must move the fingerprint
    // exactly as a rebind does, or "the adapter's copy is out of date" would
    // miss a sensitivity change.
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD_MOUSE, &content);
    content.mouse.sensitivity_x = 1234u;
    content.mouse.sensitivity_y = 777u;
    content.mouse.recenter_ms = 42u;
    content.mouse.invert_y = 1u;
    content.mouse.anti_deadzone = 17u;
    emit_one_vector(out, "kbm-mouse-only", NS2_KBM_LAYOUT_KEYBOARD_MOUSE,
                    &content, false);

    // A binding explicitly cleared to NONE, which is stored rather than dropped.
    ns2_kbm_template_default(NS2_KBM_LAYOUT_KEYBOARD, &content);
    {
        ns2_kbm_source_t source = {NS2_KBM_SRC_KEY, 0x1Au};
        (void)ns2_kbm_set_binding(&content, NS2_KBM_LAYOUT_KEYBOARD, source,
                                  NS2_DST_NONE);
    }
    emit_one_vector(out, "kb-cleared-binding", NS2_KBM_LAYOUT_KEYBOARD, &content,
                    false);

    fprintf(out, "\n  ]");
}

// ---------------------------------------------------------------------------
// The canonical default mappings, for OFFLINE clients
// ---------------------------------------------------------------------------
// A companion must be able to create and edit a profile with no adapter
// connected -- the library belongs to the user, not to a device. To DRAW a
// mapping it needs the canonical table each layout's sparse overrides are
// applied against, and that table lives here in firmware.
//
// Publishing it as a generated fixture is what makes the app work offline
// without either party inventing a second copy. The client embeds this file and
// a parity test asserts it still matches.
static void emit_default_mappings(void) {
    FILE *out = fopen("tools/fixtures/management/kbm-default-mappings.json", "w");
    if (!out) return;

    fprintf(out,
            "{\n  \"note\": \"GENERATED by tools/test_ns2_kbm_commands.c from the"
            " firmware's canonical default tables (KBM_DEFAULT_* in"
            " src/ns2_kbm.c). Companions embed this so a profile can be created"
            " and edited with no adapter connected: a local profile stores only"
            " sparse overrides, and this is what they are applied against. Do not"
            " hand-edit.\",\n  \"layouts\": {\n");

    for (unsigned l = 0; l < NS2_KBM_LAYOUT_COUNT; ++l) {
        ns2_kbm_layout_t layout = (ns2_kbm_layout_t)l;
        ns2_kbm_content_t content;
        ns2_kbm_template_default(layout, &content);

        static ns2_kbm_effective_t effective[NS2_KBM_MAX_EFFECTIVE];
        uint16_t total = ns2_kbm_effective_bindings(&content, layout, effective,
                                                    NS2_KBM_MAX_EFFECTIVE);

        fprintf(out, "    \"%s\": {\n      \"bindings\": [\n",
                ns2_kbm_layout_name(layout));
        for (uint16_t i = 0; i < total; ++i) {
            char source[12];
            ns2_kbm_source_format(effective[i].source, source, sizeof(source));
            fprintf(out, "        {\"src\":\"%s\",\"dst\":\"%s\"}%s\n", source,
                    ns2_kbm_destination_name(effective[i].destination),
                    i + 1u < total ? "," : "");
        }

        // The default mouse block travels with them: a profile created offline
        // must start from the same tuning the adapter would have given it.
        fprintf(out,
                "      ],\n      \"mouse\": {\"sensitivityX\":%u,"
                "\"sensitivityY\":%u,\"velocityWindowMs\":%u,\"invertX\":%s,"
                "\"invertY\":%s,\"antiDeadzone\":%u}\n    }%s\n",
                (unsigned)content.mouse.sensitivity_x,
                (unsigned)content.mouse.sensitivity_y,
                (unsigned)content.mouse.recenter_ms,
                content.mouse.invert_x ? "true" : "false",
                content.mouse.invert_y ? "true" : "false",
                (unsigned)content.mouse.anti_deadzone,
                l + 1u < NS2_KBM_LAYOUT_COUNT ? "," : "");
    }

    fprintf(out, "  }\n}\n");
    fclose(out);
}

int main(void) {
    printf("== ns2_kbm_commands ==\n");

    // The corpus is the exact firmware output the client tests replay. Written
    // on every run so it cannot drift from the formatter it documents.
    FILE *corpus = fopen("tools/fixtures/management/kbm-wire-corpus.json", "w");
    if (corpus) {
        fprintf(corpus,
                "{\n  \"note\": \"GENERATED by tools/test_ns2_kbm_commands.c."
                " Exact replies from the real firmware formatters"
                " (src/ns2_kbm_commands.c) for a worst-case configuration:"
                " a full profile library, maximum-length names, and every"
                " override slot bound to the longest destination name. The"
                " Windows and Android integration tests replay these bytes"
                " through their real clients, so all three implementations"
                " check against one authority. Do not hand-edit.\",\n");
        fprintf(corpus, "  \"replyMaxBytes\": %u,\n",
                (unsigned)NS2_KBM_REPLY_MAX_BYTES);
        fprintf(corpus, "  \"replies\": [\n");
    }

    // Property tests first, on whatever content each needs. The corpus comes
    // from one coherent adapter afterwards, so its records agree with each
    // other; a fixture stitched together from these tests would describe a
    // state no adapter could be in.
    test_walk_reconstructs_every_mapping_exactly_once(NULL);
    test_a_short_page_resumes_where_it_stopped();
    test_one_item_always_fits();
    test_a_cursor_past_the_end_terminates();
    test_profile_library_walk(NULL);
    test_active_fits_without_pagination(NULL);
    test_switch_list_fits_without_pagination(NULL);
    test_stored_profile_mapping_walks_identically(NULL);
    emit_corpus(corpus);

    if (corpus) {
        fprintf(corpus, "\n  ],\n");
        emit_fingerprint_vectors(corpus);
        fprintf(corpus, "\n}\n");
        fclose(corpus);
    }

    emit_default_mappings();

    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks,
           failures);
    return failures ? 1 : 0;
}
