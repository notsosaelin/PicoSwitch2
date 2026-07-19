// ds5_bt.c - Sony DualSense Bluetooth Driver
// Handles PS5 DualSense controllers over Bluetooth
//
// Reference: https://controllers.fandom.com/wiki/Sony_DualSense
// BT reports have similar structure to USB but with different report IDs
// BT output reports require CRC32

#include "ds5_bt.h"
#include "ds5_audio_packet.h"
#include "ds5_output.h"
#include "ds5_audio_bridge.h"
#include "bt/bthid/bthid.h"
#include "bt/transport/bt_transport.h"
#include "core/input_event.h"
#include "core/router/router.h"
#include "core/buttons.h"
#include "core/services/players/manager.h"
#include "core/services/players/feedback.h"
#include "controller_battery.h"
#include "platform/platform.h"
#include <string.h>
#include <stdio.h>

// Player LED patterns for DS5 (5 LEDs in a row)
// Pattern is a bitmask: bit 0=leftmost, bit 4=rightmost
static const uint8_t PLAYER_LED_PATTERNS[] = {
    0x04,   // Player 1: center LED (--*--)
    0x0A,   // Player 2: left+right of center (-*-*-)
    0x15,   // Player 3: outer + center (*-*-*)
    0x1B,   // Player 4: all but center (**-**)
    0x1F,   // Player 5: all LEDs (*****)
};

// ============================================================================
// DS5 CONSTANTS
// ============================================================================

// Report IDs
#define DS5_REPORT_BT_INPUT     0x31    // Full BT input report
#define DS5_REPORT_USB_INPUT    0x01    // USB input report (fallback)
#define DS5_REPORT_BT_OUTPUT    0x31    // BT output report

// ============================================================================
// DS5 REPORT STRUCTURE
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t x1, y1;         // Left stick
    uint8_t x2, y2;         // Right stick
    uint8_t l2_trigger;     // L2 analog
    uint8_t r2_trigger;     // R2 analog
    uint8_t counter;        // Report counter / sequence number

    struct {
        uint8_t dpad     : 4;   // Hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=released
        uint8_t square   : 1;
        uint8_t cross    : 1;
        uint8_t circle   : 1;
        uint8_t triangle : 1;
    };

    struct {
        uint8_t l1     : 1;
        uint8_t r1     : 1;
        uint8_t l2     : 1;
        uint8_t r2     : 1;
        uint8_t create : 1;     // Share/Create button
        uint8_t option : 1;
        uint8_t l3     : 1;
        uint8_t r3     : 1;
    };

    struct {
        uint8_t ps       : 1;   // 0x01 PlayStation button
        uint8_t tpad     : 1;   // 0x02 Touchpad click
        uint8_t mute     : 1;   // 0x04 Mute button
        uint8_t edge_res : 1;   // 0x08 (unused)
        uint8_t fn_left  : 1;   // 0x10 DualSense Edge: left function button
        uint8_t fn_right : 1;   // 0x20 DualSense Edge: right function button
        uint8_t paddle_left  : 1; // 0x40 DualSense Edge: left back paddle
        uint8_t paddle_right : 1; // 0x80 DualSense Edge: right back paddle
    };

    uint8_t reserved1;          // 4th button byte

    // Extended data for motion (matches Linux kernel hid-playstation.c)
    uint8_t reserved2[4];       // Timestamp/padding bytes
    int16_t gyro[3];            // x, y, z (pitch, yaw, roll)
    int16_t accel[3];           // x, y, z
    uint32_t sensor_timestamp;  // Sensor timestamp (0.33µs units)
    uint8_t  reserved3;         // Temperature / reserved
    // Touchpad: 4 bytes per finger (matches dualsense_touch_point in hid-playstation.c)
    // Finger 1 at struct offset 32, Finger 2 at struct offset 36
    struct { uint8_t tpad_f1_count : 7; uint8_t tpad_f1_down : 1; };
    uint8_t  tpad_f1_pos[3];
    struct { uint8_t tpad_f2_count : 7; uint8_t tpad_f2_down : 1; };
    uint8_t  tpad_f2_pos[3];
} ds5_input_report_t;

// DS5 BT output report for LED/rumble
typedef struct __attribute__((packed)) {
    uint8_t report_id;          // 0x31
    uint8_t seq_tag;            // Sequence tag (upper nibble)
    uint8_t tag;                // 0x10 for BT

    uint8_t valid_flag0;        // Feature flags
    uint8_t valid_flag1;
    uint8_t valid_flag2;

    uint8_t rumble_right;       // High frequency motor
    uint8_t rumble_left;        // Low frequency motor

    uint8_t headphone_volume;
    uint8_t speaker_volume;
    uint8_t mic_volume;

    uint8_t audio_flags;
    uint8_t mute_flags;

    uint8_t trigger_r[11];      // Right trigger haptics
    uint8_t trigger_l[11];      // Left trigger haptics

    uint8_t reserved1[6];

    uint8_t valid_flag3;

    uint8_t reserved2[2];

    uint8_t lightbar_setup;     // LED setup flag
    uint8_t led_brightness;
    uint8_t player_led;         // Player indicator LEDs

    uint8_t lightbar_r;
    uint8_t lightbar_g;
    uint8_t lightbar_b;
} ds5_bt_output_report_t;

// ============================================================================
// DRIVER DATA
// ============================================================================

typedef struct {
    input_event_t event;
    bool initialized;
    uint8_t activation_state;
    uint32_t activation_time;
    uint8_t output_seq;
#ifdef NS2_DS5_AUDIO
    uint8_t audio_packet_counter;
    uint8_t audio_control_state;
    uint32_t audio_control_time;
    bool audio_mic_status_known;
    bool audio_mic_status;
    bool headset_connected;
    bool audio_speaker_control_known;
    bool audio_speaker_muted;
    uint8_t audio_speaker_volume;
    bool audio_headset_path_active;
    bool audio_haptics_active;
#endif

    // Current feedback state (for change detection)
    uint8_t rumble_left;
    uint8_t rumble_right;
    uint8_t led_r, led_g, led_b;
    uint8_t player_led;

    // Touchpad swipe tracking
    uint16_t tpad_last_pos;
    bool tpad_dragging;
} ds5_bt_data_t;

static ds5_bt_data_t ds5_data[BTHID_MAX_DEVICES];

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static bool ds5_send_output(bthid_device_t* device, bool initialize_compat,
                            bool setup_lightbar,
                            bool update_rumble, bool update_leds,
                            uint8_t rumble_left, uint8_t rumble_right,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t player_led)
{
    ds5_bt_data_t* ds5 = (ds5_bt_data_t*)device->driver_data;
    if (!ds5) return false;

    ds5_output_state_t state = {
        .initialize_compat = initialize_compat,
        .setup_lightbar = setup_lightbar,
        .update_rumble = update_rumble,
        .update_leds = update_leds,
        .rumble_left = rumble_left,
        .rumble_right = rumble_right,
        .led_r = r,
        .led_g = g,
        .led_b = b,
        .player_leds = player_led,
    };

    // BTstack's transport consumes the transaction byte, extracts report ID
    // 0x31, and copies the remaining 77 bytes before this function returns.
    uint8_t buf[1 + DS5_BT_OUTPUT_REPORT_LEN];
    buf[0] = 0xA2;  // DATA | OUTPUT
    ds5_build_bt_output_report(ds5->output_seq, &state, &buf[1]);

    if (!bt_send_interrupt(device->conn_index, buf, sizeof(buf))) {
        printf("[DS5_BT] Output queue failed; retaining pending feedback\n");
        return false;
    }

    ds5->output_seq = (uint8_t)((ds5->output_seq + 1u) & 0x0Fu);
    if (update_rumble) {
        ds5->rumble_left = rumble_left;
        ds5->rumble_right = rumble_right;
    }
    if (update_leds) {
        ds5->led_r = r;
        ds5->led_g = g;
        ds5->led_b = b;
        ds5->player_led = player_led;
    }

    printf("[DS5_BT] Output queued: compat=%d lightbar_setup=%d rumble=%d L=%d R=%d leds=%d\n",
           initialize_compat, setup_lightbar, update_rumble,
           rumble_left, rumble_right, update_leds);
    return true;
}

#ifdef NS2_DS5_AUDIO
static void ds5_audio_task(bthid_device_t *device, ds5_bt_data_t *ds5,
                           bool run_codec) {
    if (!ds5_audio_bridge_owns_connection(device->conn_index)) return;
    if (run_codec) ds5_audio_bridge_codec_task();

    bool speaker_muted;
    uint8_t speaker_volume;
    ds5_audio_bridge_get_speaker_control(&speaker_muted, &speaker_volume);

    // Report 0x39 is ignored until the extended 0x32 state transaction enables
    // AudioControl. The same transaction supplies non-zero volume and
    // unmuted/muted state rather than relying on values retained from a
    // previous host. Hardware also requires AudioControl 0x02 on headset
    // insertion. Latch that path for the rest of this Bluetooth connection:
    // sending the reverse 0x30 transaction on removal was observed to poison
    // the console session, while report 0x39 already selects the live output
    // destination per audio block. Preserve activation as an ordered step. The
    // direct-L2CAP transport reports queue acceptance rather than completed
    // transmission, so mirror the existing LED activation state machine's
    // settling delay before allowing status/stream traffic to follow it.
    bool const headset_path_pending =
        ds5->headset_connected && !ds5->audio_headset_path_active;
    bool const speaker_control_changed =
        !ds5->audio_speaker_control_known ||
        ds5->audio_speaker_muted != speaker_muted ||
        ds5->audio_speaker_volume != speaker_volume ||
        headset_path_pending;
    if (ds5->audio_control_state == 0 ||
        (ds5->audio_control_state == 2 && speaker_control_changed)) {
        static uint8_t control_buf[1 + DS5_AUDIO_CONTROL_REPORT_LEN];
        bool const use_headset_path =
            ds5->audio_headset_path_active || ds5->headset_connected;
        control_buf[0] = 0xA2;
        ds5_audio_build_control_report(ds5->output_seq, use_headset_path,
                                       speaker_muted, speaker_volume,
                                       control_buf + 1);
        if (bt_send_interrupt(device->conn_index, control_buf,
                              sizeof(control_buf))) {
            ds5->output_seq = (uint8_t)((ds5->output_seq + 1u) & 0x0Fu);
            ds5->audio_speaker_control_known = true;
            ds5->audio_speaker_muted = speaker_muted;
            ds5->audio_speaker_volume = speaker_volume;
            ds5->audio_headset_path_active = use_headset_path;
            if (ds5->audio_control_state == 0) {
                ds5->audio_control_state = 1;
                ds5->audio_control_time = platform_time_ms();
            }
        }
        return;
    }
    if (ds5->audio_control_state == 1) {
        if (platform_time_ms() - ds5->audio_control_time < 30u) return;
        ds5->audio_control_state = 2;
    }

    bool const mic_active = ds5_audio_bridge_mic_active();
    if (!ds5->audio_mic_status_known || ds5->audio_mic_status != mic_active) {
        static uint8_t status_buf[1 + DS5_AUDIO_MIC_STATUS_REPORT_LEN];
        status_buf[0] = 0xA2;
        ds5_audio_build_mic_status_report(ds5->output_seq, mic_active,
                                          status_buf + 1);
        if (bt_send_interrupt(device->conn_index, status_buf,
                              sizeof(status_buf))) {
            ds5->output_seq = (uint8_t)((ds5->output_seq + 1u) & 0x0Fu);
            ds5->audio_mic_status = mic_active;
            ds5->audio_mic_status_known = true;
        }
        // Keep this state transaction ordered ahead of the first stream
        // packet. The direct L2CAP path has only a current+next queue.
        return;
    }

    static uint8_t frame_a[DS5_AUDIO_OPUS_FRAME_LEN];
    static uint8_t frame_b[DS5_AUDIO_OPUS_FRAME_LEN];
    if (!ds5_audio_bridge_peek_speaker_pair(frame_a, frame_b)) return;

    static uint8_t stream_buf[1 + DS5_AUDIO_STREAM_REPORT_LEN];
    uint8_t const next_packet_counter =
        (uint8_t)(ds5->audio_packet_counter + 2u);
    // Both DS5Dongle's working report 0x39 path and daidr's independent report
    // 0x36 path use 0x40 for the controller audio-buffer fields.
    uint8_t const audio_buffer_length = 64u;
    stream_buf[0] = 0xA2;
    ds5_audio_build_stream_report(
        ds5->output_seq, next_packet_counter, mic_active,
        ds5->headset_connected, ds5->rumble_left, ds5->rumble_right,
        audio_buffer_length,
        frame_a, frame_b, stream_buf + 1);
    if (bt_send_interrupt(device->conn_index, stream_buf,
                          sizeof(stream_buf))) {
        ds5->output_seq = (uint8_t)((ds5->output_seq + 1u) & 0x0Fu);
        ds5->audio_packet_counter = next_packet_counter;
        ds5_audio_bridge_commit_speaker_pair();
    }
}

static void ds5_audio_service_all(bool run_codec) {
    for (unsigned i = 0; i < BTHID_MAX_DEVICES; ++i) {
        ds5_bt_data_t *ds5 = &ds5_data[i];
        if (!ds5->initialized || ds5->activation_state != 3) continue;
        bthid_device_t *device = bthid_get_device(ds5->event.dev_addr);
        if (device && device->driver_data == ds5)
            ds5_audio_task(device, ds5, run_codec);
    }
}

void ds5_bt_audio_service(void) {
#ifdef NS2_DS5_AUDIO_LIVE_OPUS
    // Live encoding runs in core1's foreground worker. This background
    // BTstack safe point only activates audio and transports completed pairs.
    ds5_audio_service_all(false);
#else
    ds5_audio_service_all(true);
#endif
}

void ds5_bt_audio_report_service(void) {
#ifdef NS2_DS5_AUDIO_TEST_TONE
    // The fixed diagnostic has no encoder: its "codec" task is only a time
    // comparison and can safely run inside the inbound report safe point.
    ds5_audio_service_all(true);
#else
    // Live Opus is deliberately excluded from the deep receive callback.
    // Already encoded pairs may still be transported from this safe point.
    ds5_audio_service_all(false);
#endif
}
#else
void ds5_bt_audio_service(void) {}
void ds5_bt_audio_report_service(void) {}
#endif

// ============================================================================
// DRIVER IMPLEMENTATION
// ============================================================================

static bool ds5_match(const char* device_name, const uint8_t* class_of_device,
                      uint16_t vendor_id, uint16_t product_id, bool is_ble)
{
    (void)class_of_device;
    (void)is_ble;

    // VID/PID match (highest priority) - Sony vendor ID = 0x054C
    // DualSense = 0x0CE6, DualSense Edge = 0x0DF2
    if (vendor_id == 0x054C && (product_id == 0x0CE6 || product_id == 0x0DF2)) {
        return true;
    }

    // Name-based match (fallback if SDP query didn't return VID/PID)
    if (device_name) {
        if (strstr(device_name, "DualSense") != NULL) {
            return true;
        }
        if (strstr(device_name, "PS5 Controller") != NULL) {
            return true;
        }
    }

    return false;
}

static bool ds5_init(bthid_device_t* device)
{
    printf("[DS5_BT] Init for device: %s\n", device->name);

    // Find free data slot
    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (!ds5_data[i].initialized) {
            init_input_event(&ds5_data[i].event);
            ds5_data[i].initialized = true;
            ds5_data[i].activation_state = 0;
            ds5_data[i].activation_time = 0;
            ds5_data[i].output_seq = 0;
#ifdef NS2_DS5_AUDIO
            ds5_data[i].audio_packet_counter = 0;
            ds5_data[i].audio_control_state = 0;
            ds5_data[i].audio_control_time = 0;
            ds5_data[i].audio_mic_status_known = false;
            ds5_data[i].audio_mic_status = false;
            ds5_data[i].headset_connected = false;
            ds5_data[i].audio_speaker_control_known = false;
            ds5_data[i].audio_speaker_muted = false;
            ds5_data[i].audio_speaker_volume = 100;
            ds5_data[i].audio_headset_path_active = false;
            ds5_data[i].audio_haptics_active = false;
#endif
            ds5_data[i].rumble_left = 0;
            ds5_data[i].rumble_right = 0;
            ds5_data[i].led_r = 0;
            ds5_data[i].led_g = 0;
            ds5_data[i].led_b = 64;  // Default blue
            ds5_data[i].player_led = PLAYER_LED_PATTERNS[0];
            ds5_data[i].tpad_last_pos = 0;
            ds5_data[i].tpad_dragging = false;

            ds5_data[i].event.type = INPUT_TYPE_GAMEPAD;
            ds5_data[i].event.transport = INPUT_TRANSPORT_BT_CLASSIC;
            ds5_data[i].event.dev_addr = device->conn_index;
            ds5_data[i].event.instance = 0;
            ds5_data[i].event.button_count = 14;
            ds5_data[i].event.has_motion = true;

            device->driver_data = &ds5_data[i];
#ifdef NS2_DS5_AUDIO
            ds5_audio_bridge_connect(device->conn_index);
#endif

            // Activation happens in task (state machine with delays)
            return true;
        }
    }

    return false;
}

static bool ds5_process_debug_done = false;

static void ds5_process_report(bthid_device_t* device, const uint8_t* data, uint16_t len)
{
    ds5_bt_data_t* ds5 = (ds5_bt_data_t*)device->driver_data;

    if (!ds5 || len < 1) return;

    // Debug: print first report received
    if (!ds5_process_debug_done) {
        printf("[DS5_BT] Process report: len=%d, data[0]=0x%02X\n", len, data[0]);
        ds5_process_debug_done = true;
    }

    uint8_t report_id = data[0];
    const uint8_t* report_data = NULL;
    uint16_t report_len = 0;

#ifdef NS2_DS5_AUDIO
    // Audio mode multiplexes 71-byte microphone Opus frames into report 0x31.
    // Treating their payload as sticks/buttons caused the severe random-input
    // spam seen in the first speaker hardware experiment.
    if (ds5_audio_is_mic_input_report(data, len)) {
        // Microphone decode/USB return is a later milestone. Dropping the frame
        // here is intentional; report-boundary maintenance has already run in
        // bthid before dispatch, so BOOTSEL still receives service.
        return;
    }
#endif

    if (report_id == DS5_REPORT_BT_INPUT && len >= 12) {
        // Full BT report: report_id (1) + header (1) = skip 2 bytes
        report_data = data + 2;
        report_len = len - 2;
    } else if (report_id == DS5_REPORT_USB_INPUT && len >= 10) {
        // USB-style report: skip just report_id
        report_data = data + 1;
        report_len = len - 1;
    } else {
        // Unknown report format
        printf("[DS5_BT] Unknown report: len=%d, data[0]=0x%02X\n", len, data[0]);
        return;
    }

    if (report_len < sizeof(ds5_input_report_t)) {
        return;
    }

    const ds5_input_report_t* rpt = (const ds5_input_report_t*)report_data;

    // Parse D-pad (hat format)
    bool dpad_up    = (rpt->dpad == 0 || rpt->dpad == 1 || rpt->dpad == 7);
    bool dpad_right = (rpt->dpad >= 1 && rpt->dpad <= 3);
    bool dpad_down  = (rpt->dpad >= 3 && rpt->dpad <= 5);
    bool dpad_left  = (rpt->dpad >= 5 && rpt->dpad <= 7);

    // Build button state (inverted: 0 = pressed in USBR convention)
    uint32_t buttons = 0x00000000;

    if (dpad_up)       buttons |= JP_BUTTON_DU;
    if (dpad_down)     buttons |= JP_BUTTON_DD;
    if (dpad_left)     buttons |= JP_BUTTON_DL;
    if (dpad_right)    buttons |= JP_BUTTON_DR;
    if (rpt->cross)    buttons |= JP_BUTTON_B1;
    if (rpt->circle)   buttons |= JP_BUTTON_B2;
    if (rpt->square)   buttons |= JP_BUTTON_B3;
    if (rpt->triangle) buttons |= JP_BUTTON_B4;
    if (rpt->l1)       buttons |= JP_BUTTON_L1;
    if (rpt->r1)       buttons |= JP_BUTTON_R1;
    if (rpt->l2)       buttons |= JP_BUTTON_L2;
    if (rpt->r2)       buttons |= JP_BUTTON_R2;
    if (rpt->create)   buttons |= JP_BUTTON_S1;
    if (rpt->option)   buttons |= JP_BUTTON_S2;
    if (rpt->l3)       buttons |= JP_BUTTON_L3;
    if (rpt->r3)       buttons |= JP_BUTTON_R3;
    if (rpt->ps)       buttons |= JP_BUTTON_A1;
    if (rpt->tpad)     buttons |= JP_BUTTON_A2;
    if (rpt->mute)     buttons |= JP_BUTTON_A3;

    // DualSense Edge extra inputs: same button byte as PS/touchpad/mute, upper bits
    // (0 on a standard DualSense, so this is safe to parse unconditionally). Offsets
    // per SDL's HIDAPI_DriverPS5 (0x10 FnL, 0x20 FnR, 0x40 left paddle, 0x80 right).
    // Back paddles and the two front Fn buttons all default to the Switch grips
    // GL/GR. The Fn buttons keep distinct source bits (A4/A5, not touchpad A2 /
    // mute A3) so they still show as their own inputs and can be reassigned
    // separately in the config view. Seam defaults (NS2_DEFAULT_MAP, config.c):
    // paddles->GL/GR, FnL->GL, FnR->GR.
    if (rpt->paddle_left)  buttons |= JP_BUTTON_L4;   // -> GL
    if (rpt->paddle_right) buttons |= JP_BUTTON_R4;   // -> GR
    if (rpt->fn_left)      buttons |= JP_BUTTON_A4;   // Fn L -> GL (default)
    if (rpt->fn_right)     buttons |= JP_BUTTON_A5;   // Fn R -> GR (default)

    // Update event
    ds5->event.buttons = buttons;

    // Analog sticks (HID convention: 0=up, 255=down)
    ds5->event.analog[ANALOG_LX] = rpt->x1;
    ds5->event.analog[ANALOG_LY] = rpt->y1;
    ds5->event.analog[ANALOG_RX] = rpt->x2;
    ds5->event.analog[ANALOG_RY] = rpt->y2;

    // Triggers
    ds5->event.analog[ANALOG_L2] = rpt->l2_trigger;
    ds5->event.analog[ANALOG_R2] = rpt->r2_trigger;

    // Motion data (DS5 has full 3-axis gyro and accel)
    // Check if we have enough data for motion
    if (report_len >= sizeof(ds5_input_report_t)) {
        ds5->event.has_motion = true;
        ds5->event.accel[0] = rpt->accel[0];
        ds5->event.accel[1] = rpt->accel[1];
        ds5->event.accel[2] = rpt->accel[2];
        ds5->event.gyro[0] = rpt->gyro[0];
        ds5->event.gyro[1] = rpt->gyro[1];
        ds5->event.gyro[2] = rpt->gyro[2];
    } else {
        ds5->event.has_motion = false;
    }

    // Battery: status byte at report_data[52].
    if (report_len > 52) {
        controller_battery_t battery;
        if (controller_battery_decode_ds5(report_data[52], &battery)) {
            input_event_set_native_battery(&ds5->event, battery.level,
                                           battery.charging);
        }
    }

#ifdef NS2_DS5_AUDIO
    // Headset insertion is reported in the full 0x31 status byte. The
    // DualSense audio transport selects speaker (0x13) or headphones (0x16)
    // when the next paired Opus report is built. Preserve headphone-vs-headset
    // separately so the emulated Pro Controller 2 can expose the matching
    // Nintendo state only while the physical jack is occupied.
    if (report_id == DS5_REPORT_BT_INPUT) {
        ds5->event.headset_state = ds5_audio_headset_state(data, len);
        ds5->headset_connected =
            ds5->event.headset_state != CONTROLLER_HEADSET_NONE;
    }
#endif

    // Touchpad (only in full 0x31 reports that include touch fields)
    if (report_len >= sizeof(ds5_input_report_t)) {
        uint16_t tx = ((rpt->tpad_f1_pos[1] & 0x0f) << 8) | (rpt->tpad_f1_pos[0] & 0xff);
        uint16_t ty = ((rpt->tpad_f1_pos[1] & 0xf0) >> 4) | ((rpt->tpad_f1_pos[2] & 0xff) << 4);
        uint16_t tx2 = ((rpt->tpad_f2_pos[1] & 0x0f) << 8) | (rpt->tpad_f2_pos[0] & 0xff);
        uint16_t ty2 = ((rpt->tpad_f2_pos[1] & 0xf0) >> 4) | ((rpt->tpad_f2_pos[2] & 0xff) << 4);

        // (Touchpad-halves were previously faked as L4/R4 back-paddles; the DualSense
        // Edge now reports real paddles above, so that hack is gone. Touchpad click
        // stays mapped to A2/Capture via the button parse.)

        // Touchpad swipe delta (horizontal)
        int8_t touchpad_delta_x = 0;
        if (!rpt->tpad_f1_down) {
            if (ds5->tpad_dragging) {
                int16_t delta = (int16_t)tx - (int16_t)ds5->tpad_last_pos;
                if (delta > 12) delta = 12;
                if (delta < -12) delta = -12;
                touchpad_delta_x = (int8_t)delta;
            }
            ds5->tpad_last_pos = tx;
            ds5->tpad_dragging = true;
        } else {
            ds5->tpad_dragging = false;
        }
        ds5->event.delta_x = touchpad_delta_x;

        // Touch coordinates for SInput pass-through
        ds5->event.touch[0].x = tx;
        ds5->event.touch[0].y = ty;
        ds5->event.touch[0].active = !rpt->tpad_f1_down;
        ds5->event.touch[1].x = tx2;
        ds5->event.touch[1].y = ty2;
        ds5->event.touch[1].active = !rpt->tpad_f2_down;
        ds5->event.has_touch = true;
    }

    // Submit to router
    router_submit_input(&ds5->event);
}

static void ds5_task(bthid_device_t* device)
{
    ds5_bt_data_t* ds5 = (ds5_bt_data_t*)device->driver_data;
    if (!ds5) return;

    uint32_t now = platform_time_ms();

    // State machine for activation with delays
    switch (ds5->activation_state) {
        case 0:  // Wait a moment then send initial LED
            ds5->activation_state = 1;
            ds5->activation_time = now;
            break;

        case 1:  // Wait 100ms then send daidr-compatible initialization + LEDs
            if (now - ds5->activation_time >= 100) {
                int player_idx = find_player_index(ds5->event.dev_addr, ds5->event.instance);
                int idx = (player_idx >= 0 && player_idx < 7) ? player_idx : 0;
                feedback_state_t* fb = feedback_get_state(idx);
                uint8_t r = fb ? fb->led.r : 0;
                uint8_t g = fb ? fb->led.g : 0;
                uint8_t b = fb ? fb->led.b : 0;
                int player_num = idx;
                if (fb && fb->led.pattern != 0) {
                    player_num = 0;
                    for (int p = 1; p <= 7; p++) {
                        if (fb->led.pattern == PLAYER_LEDS[p]) {
                            player_num = p - 1;
                            break;
                        }
                    }
                }
                int pat_idx = (player_num < 5) ? player_num : player_num % 5;
                // Keep the hardware-proven compatibility flags, but also issue
                // the one-time LIGHT_OUT setup required before RGB writes.
                if (ds5_send_output(device, true, true, false, false, 0, 0,
                                    r, g, b,
                                    PLAYER_LED_PATTERNS[pat_idx])) {
                    ds5->activation_state = 2;
                    ds5->activation_time = now;
                }
            }
            break;

        case 2:  // Program RGB/player LEDs after the setup packet has left
            if (now - ds5->activation_time >= 30) {
                int player_idx = find_player_index(ds5->event.dev_addr, ds5->event.instance);
                int idx = (player_idx >= 0 && player_idx < 7) ? player_idx : 0;
                feedback_state_t* fb = feedback_get_state(idx);
                uint8_t r = fb ? fb->led.r : 0;
                uint8_t g = fb ? fb->led.g : 0;
                uint8_t b = fb ? fb->led.b : 0;
                int player_num = idx;
                if (fb && fb->led.pattern != 0) {
                    player_num = 0;
                    for (int p = 1; p <= 7; p++) {
                        if (fb->led.pattern == PLAYER_LEDS[p]) {
                            player_num = p - 1;
                            break;
                        }
                    }
                }
                int pat_idx = (player_num < 5) ? player_num : player_num % 5;
                if (ds5_send_output(device, false, false, false, true, 0, 0,
                                    r, g, b, PLAYER_LED_PATTERNS[pat_idx])) {
                    ds5->activation_state = 3;
                }
            }
            break;

        case 3:  // Activated - monitor feedback system for rumble/LED updates
            {
                int player_idx = find_player_index(ds5->event.dev_addr, ds5->event.instance);
                if (player_idx >= 0) {
                    feedback_state_t* fb = feedback_get_state(player_idx);
                    if (!fb) break;

                    bool rumble_update = false;
                    bool led_update = false;
                    uint8_t r = ds5->led_r;
                    uint8_t g = ds5->led_g;
                    uint8_t b = ds5->led_b;
                    uint8_t player_led = ds5->player_led;
                    uint8_t rumble_left = ds5->rumble_left;
                    uint8_t rumble_right = ds5->rumble_right;
#ifdef NS2_DS5_AUDIO
                    bool const audio_haptics_active =
                        ds5->headset_connected ||
                        ds5_audio_bridge_speaker_requested();
                    bool const audio_haptics_ended =
                        ds5->audio_haptics_active && !audio_haptics_active;
                    ds5->audio_haptics_active = audio_haptics_active;
#endif

                    // Calculate player LED from pattern (like DS3)
                    // DS5 has separate player LED bar and RGB lightbar
                    uint8_t calc_player_led;
                    if (fb->led.pattern != 0) {
                        // Map feedback pattern to player number via PLAYER_LEDS[] lookup
                        int player_num = 0;
                        for (int p = 1; p <= 7; p++) {
                            if (fb->led.pattern == PLAYER_LEDS[p]) {
                                player_num = p - 1;
                                break;
                            }
                        }
                        int pat_idx = (player_num < 5) ? player_num : player_num % 5;
                        calc_player_led = PLAYER_LED_PATTERNS[pat_idx];
                    } else {
                        // Default to player index
                        int idx = (player_idx < 5) ? player_idx : player_idx % 5;
                        calc_player_led = PLAYER_LED_PATTERNS[idx];
                    }

                    // Check if player LED changed
                    if (calc_player_led != ds5->player_led) {
                        player_led = calc_player_led;
                        led_update = true;
                    }

                    // Check RGB lightbar from feedback
                    if (fb->led_dirty) {
                        // RGB is independent of the player-dot pattern. All-zero
                        // is a valid configured body colour (lightbar off).
                        r = fb->led.r;
                        g = fb->led.g;
                        b = fb->led.b;
                        player_led = calc_player_led;
                        led_update = true;
                    }

                    // Check rumble
                    if (fb->rumble_dirty) {
                        rumble_left = fb->rumble.left;
                        rumble_right = fb->rumble.right;
                        rumble_update = true;
                    }

                    // Also check if values changed (even without dirty flag)
                    if (rumble_left != ds5->rumble_left || rumble_right != ds5->rumble_right)
                        rumble_update = true;
                    if (r != ds5->led_r || g != ds5->led_g || b != ds5->led_b ||
                        player_led != ds5->player_led)
                        led_update = true;

#ifdef NS2_DS5_AUDIO
                    bool rumble_consumed_by_audio = false;
                    // Report 0x39 already has two 64-byte 3 kHz haptic PCM
                    // blocks, separate from its two Opus speaker blocks. Take
                    // ownership as soon as a headset/audio path is requested,
                    // before the first stream packet. Waiting for a successful
                    // 0x39 allowed repeated console rumble generations to keep
                    // filling the ordinary 0x31 output path, starving the
                    // activation/stream traffic required to end that fallback.
                    if (rumble_update && audio_haptics_active) {
                        ds5->rumble_left = rumble_left;
                        ds5->rumble_right = rumble_right;
                        rumble_update = false;
                        rumble_consumed_by_audio = true;
                    }
                    // When the USB host closes its audio stream, restore the
                    // established legacy path immediately using the last
                    // commanded values (including a zero-magnitude STOP).
                    if (audio_haptics_ended) rumble_update = true;

                    bool output_accepted = false;
                    if (rumble_update || led_update) {
                        output_accepted =
                            ds5_send_output(device, false, false,
                                            rumble_update, led_update,
                                            rumble_left, rumble_right,
                                            r, g, b, player_led);
                    }
                    if (output_accepted ||
                        (rumble_consumed_by_audio && !led_update)) {
                        // Only consume feedback after BTstack has accepted the
                        // report, or after live audio has taken ownership of the
                        // haptic values. A failed legacy STOP is still retried.
                        feedback_clear_dirty(player_idx);
                    }
#else
                    if ((rumble_update || led_update) &&
                        ds5_send_output(device, false, false, rumble_update, led_update,
                                        rumble_left, rumble_right, r, g, b, player_led)) {
                        // Only consume feedback after BTstack has accepted the
                        // report. A failed STOP is therefore retried instead of
                        // leaving a previous motor command latched.
                        feedback_clear_dirty(player_idx);
                    }
#endif
                }
            }
            break;
    }
}

static void ds5_disconnect(bthid_device_t* device)
{
    printf("[DS5_BT] Disconnect: %s\n", device->name);

    ds5_bt_data_t* ds5 = (ds5_bt_data_t*)device->driver_data;
    if (ds5) {
#ifdef NS2_DS5_AUDIO
        ds5_audio_bridge_disconnect(device->conn_index);
#endif
        // Clear router state first (sends zeroed input report)
        router_device_disconnected(ds5->event.dev_addr, ds5->event.instance);
        // Remove player assignment
        remove_players_by_address(ds5->event.dev_addr, ds5->event.instance);

        init_input_event(&ds5->event);
        ds5->initialized = false;
    }
}

// ============================================================================
// DRIVER STRUCT
// ============================================================================

const bthid_driver_t ds5_bt_driver = {
    .name = "Sony DualSense",
    .transports = BTHID_TRANSPORT_CLASSIC,  // DualSense BT pairing is Classic BT, not BLE
    .match = ds5_match,
    .init = ds5_init,
    .process_report = ds5_process_report,
    .task = ds5_task,
    .disconnect = ds5_disconnect,
};

void ds5_bt_register(void)
{
    bthid_register_driver(&ds5_bt_driver);
}
