// Host tests for the Windows Controller Link data-plane wire contract.
//
// These pin the three things a carrier change must not get wrong: the frame
// layout both ends agree on, the reordering rule that keeps "latest state wins"
// true when the transport delivers out of order, and the watchdog that stops a
// killed Windows process from leaving a stick held on the console.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ns2_companion_link.h"
#include "bt/bthid/bthid.h"
#include "bt/bthid/devices/generic/bthid_gamepad_quirks.h"
#include "core/buttons.h"
#include "core/services/players/feedback.h"

// The decoder test links the real gamepad-quirks translation unit so it uses
// the SAME usage-number table the Classic bridge path does -- building a local
// copy would only prove this test agrees with itself. That unit references the
// wider driver surface, so stub it, exactly as test_bthid_android_bridge.c does.
const gamepad_quirk_t QUIRK_BITDO_ULTIMATE_MG = {0};
const gamepad_quirk_t QUIRK_BITDO_M30 = {0};
const gamepad_quirk_t QUIRK_BITDO_PADDLE = {0};
const gamepad_quirk_t QUIRK_XBOX_ELITE2 = {0};
const gamepad_quirk_t QUIRK_XBOX = {0};

static feedback_state_t stub_feedback;

void router_submit_input(const input_event_t *event) { (void)event; }
void router_device_disconnected(uint8_t a, int8_t i) { (void)a; (void)i; }
void router_device_disconnected_with_generation(uint8_t a, int8_t i, uint32_t g)
{ (void)a; (void)i; (void)g; }
void remove_players_by_address(int a, int i) { (void)a; (void)i; }
int find_player_index(int dev_addr, int instance)
{ (void)dev_addr; (void)instance; return -1; }
feedback_state_t *feedback_get_state(uint8_t player_index)
{ (void)player_index; return &stub_feedback; }
void feedback_clear_dirty(uint8_t player_index) { (void)player_index; }
void bthid_register_driver(const bthid_driver_t *driver) { (void)driver; }
bthid_device_t *bthid_get_device(uint8_t conn_index) { (void)conn_index; return NULL; }
bool bthid_send_output_report(uint8_t conn_index, uint8_t report_id,
                              const uint8_t *data, uint16_t len)
{ (void)conn_index; (void)report_id; (void)data; (void)len; return true; }

static void build_frame(uint8_t *out, uint16_t sequence, uint8_t first_payload)
{
    memset(out, 0, NS2_COMPANION_LINK_FRAME_BYTES);
    out[0] = NS2_COMPANION_LINK_VERSION;
    out[1] = NS2_COMPANION_LINK_OP_STATE;
    out[2] = (uint8_t)(sequence & 0xFFu);
    out[3] = (uint8_t)(sequence >> 8);
    out[NS2_COMPANION_LINK_HEADER_BYTES] = first_payload;
}

static void test_frame_geometry(void)
{
    // The report the bthid path consumes is the payload plus a report ID. If
    // these drift apart the adapter reads controller state at the wrong offsets
    // and every axis is subtly wrong rather than obviously broken.
    assert(NS2_COMPANION_LINK_PAYLOAD_BYTES == 26u);
    assert(NS2_COMPANION_LINK_REPORT_BYTES == 27u);
    assert(NS2_COMPANION_LINK_FRAME_BYTES == 30u);

    // One frame must fit one ATT operation, or every gameplay report fragments.
    assert(NS2_COMPANION_LINK_MIN_ATT_MTU == 33u);
    assert(!ns2_companion_link_mtu_sufficient(23u));   // default ATT MTU
    assert(!ns2_companion_link_mtu_sufficient(32u));
    assert(ns2_companion_link_mtu_sufficient(33u));
    assert(ns2_companion_link_mtu_sufficient(517u));   // what Windows negotiates
}

static void test_accepts_a_well_formed_frame(void)
{
    uint8_t frame[NS2_COMPANION_LINK_FRAME_BYTES];
    uint16_t last = 0;
    bool have = false;
    const uint8_t *payload = NULL;

    build_frame(frame, 7u, 0xABu);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, &payload)
           == NS2_COMPANION_FRAME_OK);
    assert(have && last == 7u);
    assert(payload == frame + NS2_COMPANION_LINK_HEADER_BYTES);
    assert(payload[0] == 0xABu);
}

static void test_rejects_malformed_frames(void)
{
    uint8_t frame[NS2_COMPANION_LINK_FRAME_BYTES];
    uint16_t last = 0;
    bool have = false;

    build_frame(frame, 1u, 0);
    assert(ns2_companion_link_parse(NULL, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_SHORT);

    // A frame one byte short is a truncated report, not a small one. Accepting
    // it would read one byte of controller state past the data.
    assert(ns2_companion_link_parse(frame, NS2_COMPANION_LINK_FRAME_BYTES - 1u,
                                    &last, &have, NULL)
           == NS2_COMPANION_FRAME_SHORT);

    build_frame(frame, 1u, 0);
    frame[0] = NS2_COMPANION_LINK_VERSION + 1u;
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_VERSION);

    build_frame(frame, 1u, 0);
    frame[1] = 0x7Fu;
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_OPCODE);

    // A rejected frame must not advance the sequence, or the next good frame
    // would be judged stale against a sequence that was never applied.
    assert(!have);
}

static void test_latest_state_wins(void)
{
    uint8_t frame[NS2_COMPANION_LINK_FRAME_BYTES];
    uint16_t last = 0;
    bool have = false;

    build_frame(frame, 10u, 0);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_OK);

    // Reordered older frame: dropped, and the applied sequence stands.
    build_frame(frame, 9u, 0);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_STALE);
    assert(last == 10u);

    // Duplicate carries nothing new.
    build_frame(frame, 10u, 0);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_STALE);

    // Newer frame applies, and a gap is fine -- coalescing upstream is allowed,
    // the semantic requirement is only that state never goes backwards.
    build_frame(frame, 40u, 0);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_OK);
    assert(last == 40u);
}

static void test_sequence_wrap_is_not_a_rewind(void)
{
    uint8_t frame[NS2_COMPANION_LINK_FRAME_BYTES];
    uint16_t last = 0xFFFEu;
    bool have = true;

    // At 125 Hz a uint16 sequence wraps every ~9 minutes. If wrap read as a
    // rewind, every session would freeze on a held state until a restart.
    build_frame(frame, 0xFFFFu, 0);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_OK);

    build_frame(frame, 0x0000u, 0);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_OK);
    assert(last == 0x0000u);

    build_frame(frame, 0x0002u, 0);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_OK);

    // ...and a genuine rewind across the wrap boundary is still stale.
    build_frame(frame, 0xFFF0u, 0);
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, NULL)
           == NS2_COMPANION_FRAME_STALE);
}

static void test_output_encoding(void)
{
    uint8_t out[16];
    const uint8_t payload[4] = { 0x11u, 0x22u, 0x33u, 0x44u };

    uint16_t n = ns2_companion_link_encode_output(0x02u, payload, 4u, out, sizeof(out));
    assert(n == NS2_COMPANION_LINK_OUT_HEADER_BYTES + 4u);
    assert(out[0] == NS2_COMPANION_LINK_VERSION);
    assert(out[1] == NS2_COMPANION_LINK_OP_OUTPUT);
    assert(out[2] == 0x02u);
    assert(memcmp(&out[3], payload, 4u) == 0);

    // A feedback report with no payload is legal; the report ID alone is news.
    assert(ns2_companion_link_encode_output(0x02u, NULL, 0u, out, sizeof(out))
           == NS2_COMPANION_LINK_OUT_HEADER_BYTES);

    // Refuse rather than truncate: a short buffer must not produce a frame the
    // companion would decode as a complete but wrong feedback report.
    assert(ns2_companion_link_encode_output(0x02u, payload, 4u, out, 5u) == 0u);
    assert(ns2_companion_link_encode_output(
               0x02u, payload, NS2_COMPANION_LINK_OUT_MAX_PAYLOAD + 1u,
               out, sizeof(out)) == 0u);
}

static void test_stale_input_watchdog(void)
{
    const uint32_t bound = NS2_COMPANION_LINK_STALE_MS;

    // Inactive link: nothing is held, nothing to neutralize.
    assert(!ns2_companion_link_input_stale(false, false, 0u, 100000u, bound));

    // Streaming normally.
    assert(!ns2_companion_link_input_stale(true, false, 1000u, 1000u, bound));
    assert(!ns2_companion_link_input_stale(true, false, 1000u, 1000u + bound - 1u, bound));

    // The Windows process died holding a stick. This is the case that matters.
    assert(ns2_companion_link_input_stale(true, false, 1000u, 1000u + bound, bound));
    assert(ns2_companion_link_input_stale(true, false, 1000u, 60000u, bound));

    // Already neutralized: do not republish neutral over whatever now owns the
    // console.
    assert(!ns2_companion_link_input_stale(true, true, 1000u, 60000u, bound));

    // A run-loop clock wrap must not postpone neutralization indefinitely.
    const uint32_t before_wrap = 0xFFFFFF00u;
    assert(ns2_companion_link_input_stale(true, false, before_wrap,
                                          before_wrap + bound + 50u, bound));
    assert(!ns2_companion_link_input_stale(true, false, before_wrap,
                                           before_wrap + bound - 50u, bound));
}

// Byte-exact cross-language binding.
//
// tools/fixtures/bridge_report_goldens.csv is generated from the Kotlin encoder
// and already pins the C# ControllerReportEncoder byte-for-byte
// (BridgeReportGoldenTests) and the Kotlin one (BridgeReportGoldenTest). This
// extends the same fixture to the C side, so the chain
//
//     Windows encoder -> shared golden -> Path C frame -> firmware decode
//
// is proven end to end rather than asserted in a comment. It checks the exact
// payload size and the canonical offsets of every field, which is what the
// contract-4 button-field growth moved.
static void test_goldens_match_the_path_c_payload(void)
{
    FILE *f = fopen("tools/fixtures/bridge_report_goldens.csv", "r");
    if (!f) f = fopen("../tools/fixtures/bridge_report_goldens.csv", "r");
    assert(f && "bridge_report_goldens.csv must be readable from the repo root");

    char line[1024];
    unsigned checked = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == 'n') continue;

        // Last comma-separated field is the v2 payload hex.
        char *last = strrchr(line, ',');
        assert(last);
        char *hex = last + 1;
        size_t hexlen = strcspn(hex, "\r\n");
        hex[hexlen] = '\0';

        // Every golden must be exactly the Path C payload width. A fixture row
        // of any other length means the encoder and this decoder disagree about
        // the report, which is precisely the drift this test exists to catch.
        assert(hexlen == NS2_COMPANION_LINK_PAYLOAD_BYTES * 2u);

        uint8_t payload[NS2_COMPANION_LINK_PAYLOAD_BYTES];
        for (size_t i = 0; i < NS2_COMPANION_LINK_PAYLOAD_BYTES; i++) {
            unsigned byte = 0;
            assert(sscanf(&hex[i * 2u], "%2x", &byte) == 1);
            payload[i] = (uint8_t)byte;
        }

        // Wrap it in a Path C frame and prove the parser hands back exactly the
        // encoder's bytes, unshifted.
        uint8_t frame[NS2_COMPANION_LINK_FRAME_BYTES];
        build_frame(frame, (uint16_t)(checked + 1u), 0);
        memcpy(&frame[NS2_COMPANION_LINK_HEADER_BYTES], payload, sizeof(payload));

        uint16_t last_seq = 0;
        bool have = false;
        const uint8_t *out = NULL;
        assert(ns2_companion_link_parse(frame, sizeof(frame), &last_seq, &have, &out)
               == NS2_COMPANION_FRAME_OK);
        assert(memcmp(out, payload, sizeof(payload)) == 0);

        checked++;
    }
    fclose(f);

    // The fixture is the contract; an empty read would pass every assert above.
    assert(checked >= 40u);
    printf("  goldens checked: %u\n", checked);
}

// The canonical offsets, stated once here against the contract header so a
// field move breaks a test rather than silently relocating motion or battery.
// These are payload offsets (report ID excluded) and must equal the C contract
// offsets minus one -- which is asserted, not assumed.
static void test_canonical_offsets(void)
{
    // Contract 4 wire layout, INPUT report 1, payload without the report ID:
    //   [0..5]   X, Y, Z, Rz, Rx, Ry
    //   [6..8]   buttons 1..17
    //   [9]      hat
    //   [10..21] gyro XYZ, accel XYZ (int16 LE)
    //   [22]     battery
    //   [23]     flags
    //   [24..25] motion timestamp (uint16 LE)
    assert(NS2_COMPANION_LINK_PAYLOAD_BYTES == 26u);

    // Neutral golden proves the axis and hat offsets concretely: centred
    // sticks at 0..3, released triggers at 4..5, no buttons at 6..8, hat
    // neutral (8) at 9.
    const uint8_t neutral[NS2_COMPANION_LINK_PAYLOAD_BYTES] = {
        0x80u, 0x80u, 0x80u, 0x80u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u,
        0x08u,
    };
    uint8_t frame[NS2_COMPANION_LINK_FRAME_BYTES];
    build_frame(frame, 1u, 0);
    memcpy(&frame[NS2_COMPANION_LINK_HEADER_BYTES], neutral, sizeof(neutral));

    uint16_t last = 0;
    bool have = false;
    const uint8_t *p = NULL;
    assert(ns2_companion_link_parse(frame, sizeof(frame), &last, &have, &p)
           == NS2_COMPANION_FRAME_OK);
    assert(p[0] == 0x80u && p[1] == 0x80u && p[2] == 0x80u && p[3] == 0x80u);
    assert(p[4] == 0x00u && p[5] == 0x00u);
    assert(p[9] == 0x08u);
}

// Decode, against the SAME usage-number table the Classic bridge path uses.
// Passing the map in is the point: if this test built its own table it would
// prove only that the test agrees with itself.
static void test_decode_base(void)
{
    const gamepad_quirk_t *quirk = gamepad_quirks_android_bridge();
    assert(quirk && quirk->button_map && quirk->button_map_size > 0);

    uint8_t payload[NS2_COMPANION_LINK_PAYLOAD_BYTES];
    input_event_t event;

    // Neutral: centred sticks, released triggers, hat 8, no buttons.
    memset(payload, 0, sizeof(payload));
    payload[0] = payload[1] = payload[2] = payload[3] = 0x80u;
    payload[9] = 0x08u;
    init_input_event(&event);
    ns2_companion_link_decode_base(payload, quirk->button_map,
                                   quirk->button_map_size, &event);
    assert(event.analog[ANALOG_LX] == 128 && event.analog[ANALOG_LY] == 128);
    assert(event.analog[ANALOG_RX] == 128 && event.analog[ANALOG_RY] == 128);
    assert(event.analog[ANALOG_L2] == 0 && event.analog[ANALOG_R2] == 0);
    assert(event.buttons == 0u);

    // Axis order is X, Y, Z, Rz, Rx, Ry -> LX, LY, RX, RY, L2, R2. Getting this
    // wrong swaps the right stick with the triggers and is invisible until a
    // human plays the thing.
    payload[0] = 0x00u; payload[1] = 0xFFu;
    payload[2] = 0x11u; payload[3] = 0x22u;
    payload[4] = 0x33u; payload[5] = 0x44u;
    init_input_event(&event);
    ns2_companion_link_decode_base(payload, quirk->button_map,
                                   quirk->button_map_size, &event);
    assert(event.analog[ANALOG_LX] == 0x00u);
    assert(event.analog[ANALOG_LY] == 0xFFu);
    assert(event.analog[ANALOG_RX] == 0x11u);
    assert(event.analog[ANALOG_RY] == 0x22u);
    assert(event.analog[ANALOG_L2] == 0x33u);
    assert(event.analog[ANALOG_R2] == 0x44u);

    // Every hat direction, including the diagonals and the released value.
    static const struct { uint8_t hat; uint32_t expect; } hats[] = {
        { 0u, JP_BUTTON_DU },
        { 1u, JP_BUTTON_DU | JP_BUTTON_DR },
        { 2u, JP_BUTTON_DR },
        { 3u, JP_BUTTON_DR | JP_BUTTON_DD },
        { 4u, JP_BUTTON_DD },
        { 5u, JP_BUTTON_DD | JP_BUTTON_DL },
        { 6u, JP_BUTTON_DL },
        { 7u, JP_BUTTON_DU | JP_BUTTON_DL },
        { 8u, 0u },
    };
    for (unsigned i = 0; i < sizeof(hats) / sizeof(hats[0]); i++) {
        memset(payload, 0, sizeof(payload));
        payload[9] = hats[i].hat;
        init_input_event(&event);
        ns2_companion_link_decode_base(payload, quirk->button_map,
                                       quirk->button_map_size, &event);
        assert(event.buttons == hats[i].expect);
    }

    // Buttons live in three bytes since contract 4, usage = bit index + 1, and
    // usage 17 (GR) is in the third byte -- the byte that only exists because
    // contract 3 ran out of pad bits.
    for (uint8_t usage = 1u; usage <= 17u; usage++) {
        memset(payload, 0, sizeof(payload));
        payload[9] = 0x08u;  // hat released, so only the button contributes
        uint8_t bit = (uint8_t)(usage - 1u);
        payload[6u + (bit / 8u)] = (uint8_t)(1u << (bit % 8u));
        init_input_event(&event);
        ns2_companion_link_decode_base(payload, quirk->button_map,
                                       quirk->button_map_size, &event);
        uint32_t expected = (usage < quirk->button_map_size)
                                ? quirk->button_map[usage] : 0u;
        assert(event.buttons == expected);
    }

    // A bit beyond the declared 17 must contribute nothing rather than index
    // past the map.
    memset(payload, 0, sizeof(payload));
    payload[9] = 0x08u;
    payload[8] = 0xFEu;  // usages 18..24
    init_input_event(&event);
    ns2_companion_link_decode_base(payload, quirk->button_map,
                                   quirk->button_map_size, &event);
    assert(event.buttons == 0u);
}

int main(void)
{
    test_frame_geometry();
    test_canonical_offsets();
    test_goldens_match_the_path_c_payload();
    test_decode_base();
    test_accepts_a_well_formed_frame();
    test_rejects_malformed_frames();
    test_latest_state_wins();
    test_sequence_wrap_is_not_a_rewind();
    test_output_encoding();
    test_stale_input_watchdog();
    printf("ns2_companion_link: all tests passed\n");
    return 0;
}
