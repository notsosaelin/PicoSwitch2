// switch_pro_bt.c - Nintendo Switch Pro Controller Bluetooth Driver
// Handles Switch Pro and Joy-Con controllers over Bluetooth
//
// Reference: https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering

#include "switch_pro_bt.h"
#include "switch_pro_8bitdo.h"
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

// ============================================================================
// SWITCH PRO CONSTANTS
// ============================================================================

// Report IDs
#define SWITCH_REPORT_INPUT_STANDARD    0x30    // Standard full input report
#define SWITCH_REPORT_INPUT_SIMPLE      0x3F    // Simple HID mode
#define SWITCH_REPORT_SUBCMD_REPLY      0x21    // Input report + subcommand reply
#define SWITCH_REPORT_OUTPUT            0x01    // Output report with subcommand
#define SWITCH_REPORT_RUMBLE_ONLY       0x10    // Rumble only (no subcommand)

// Subcommands
#define SWITCH_SUBCMD_SET_INPUT_MODE    0x03
#define SWITCH_SUBCMD_SPI_READ          0x10    // read internal flash (IMU cal)
#define SWITCH_SUBCMD_SET_PLAYER_LED    0x30
#define SWITCH_SUBCMD_SET_HOME_LED      0x38
#define SWITCH_SUBCMD_ENABLE_IMU        0x40
#define SWITCH_SUBCMD_ENABLE_VIBRATION  0x48

// Input modes
#define SWITCH_INPUT_MODE_FULL          0x30

// Init delay between subcommands (ms)
#define SWITCH_INIT_DELAY_MS            200

// ============================================================================
// SWITCH PRO REPORT STRUCTURE
// ============================================================================

typedef struct __attribute__((packed)) {
    uint8_t report_id;      // 0x30 or 0x3F
    uint8_t timer;          // Increments by 1 per report

    uint8_t battery_conn;   // Battery level + connection info

    // Button byte 1 (right side buttons)
    struct {
        uint8_t y    : 1;
        uint8_t x    : 1;
        uint8_t b    : 1;
        uint8_t a    : 1;
        uint8_t sr_r : 1;
        uint8_t sl_r : 1;
        uint8_t r    : 1;
        uint8_t zr   : 1;
    };

    // Button byte 2 (system buttons)
    struct {
        uint8_t minus  : 1;
        uint8_t plus   : 1;
        uint8_t rstick : 1;
        uint8_t lstick : 1;
        uint8_t home   : 1;
        uint8_t capture: 1;
        uint8_t pad1   : 2;
    };

    // Button byte 3 (left side buttons + dpad)
    struct {
        uint8_t down  : 1;
        uint8_t up    : 1;
        uint8_t right : 1;
        uint8_t left  : 1;
        uint8_t sr_l  : 1;
        uint8_t sl_l  : 1;
        uint8_t l     : 1;
        uint8_t zl    : 1;
    };

    // Analog sticks (12-bit packed, 3 bytes each)
    uint8_t left_stick[3];
    uint8_t right_stick[3];

    // Vibration ack and subcommand data follow...
} switch_input_report_t;

// Simple HID report (0x3F) - used before handshake
typedef struct __attribute__((packed)) {
    uint8_t report_id;      // 0x3F

    struct {
        uint8_t b      : 1;
        uint8_t a      : 1;
        uint8_t y      : 1;
        uint8_t x      : 1;
        uint8_t l      : 1;
        uint8_t r      : 1;
        uint8_t zl     : 1;
        uint8_t zr     : 1;
    };

    struct {
        uint8_t minus  : 1;
        uint8_t plus   : 1;
        uint8_t lstick : 1;
        uint8_t rstick : 1;
        uint8_t home   : 1;
        uint8_t capture: 1;
        uint8_t pad    : 2;
    };

    uint8_t hat;            // D-pad as hat (0-7, 8=center)
    uint16_t lx, ly;        // Left stick (0-65535, center ~32768)
    uint16_t rx, ry;        // Right stick (0-65535, center ~32768)
} switch_simple_report_t;

// ============================================================================
// INIT STATE MACHINE
// ============================================================================

typedef enum {
    SWITCH_STATE_WAIT_READY,        // Wait before sending first subcommand
    SWITCH_STATE_SET_INPUT_MODE,    // Send set input mode (0x03 → 0x30)
    SWITCH_STATE_ENABLE_IMU,        // Send enable 6-axis IMU (0x40 → 0x01)
    SWITCH_STATE_READ_FACTORY_CAL,  // Send SPI read of the factory IMU cal
    SWITCH_STATE_READ_USER_CAL,     // Send SPI read of the user IMU cal
    SWITCH_STATE_ENABLE_VIBRATION,  // Send enable vibration (0x48 → 0x01)
    SWITCH_STATE_SET_PLAYER_LED,    // Send player LED (0x30)
    SWITCH_STATE_ACTIVE,            // Init complete, monitor feedback
} switch_init_state_t;

// ============================================================================
// DRIVER DATA
// ============================================================================

// Factory/user IMU calibration read out of SPI flash (see 7 below).
typedef struct {
    int16_t accel_origin[3];
    int16_t accel_sens[3];
    int16_t gyro_origin[3];
    int16_t gyro_sens[3];
    bool valid;
} sw1_imu_cal_t;

typedef struct {
    input_event_t event;
    bool initialized;
    bool full_report_mode;
    uint8_t output_seq;     // Sequence counter for output reports
    switch_init_state_t init_state;
    uint32_t init_time;     // Timestamp for init delays
    uint8_t rumble_left;    // Cached rumble state
    uint8_t rumble_right;
    sw1_imu_cal_t imu_cal;  // from SPI flash; falls back to nominal when absent
} switch_bt_data_t;

static switch_bt_data_t switch_data[BTHID_MAX_DEVICES];

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Unpack 12-bit analog value from 3-byte packed format
static uint16_t unpack_stick_12bit(const uint8_t* data, bool high)
{
    if (high) {
        // High nibble of byte 1 + all of byte 2
        return ((data[1] & 0xF0) >> 4) | (data[2] << 4);
    } else {
        // All of byte 0 + low nibble of byte 1
        return data[0] | ((data[1] & 0x0F) << 8);
    }
}

// Scale 12-bit to 8-bit
static uint8_t scale_12bit_to_8bit(uint16_t val)
{
    if (val == 0) return 1;
    return 1 + ((val * 254) / 4095);
}

static uint32_t switch_translate_vendor_paddles(const bthid_device_t *device,
                                                uint32_t buttons,
                                                uint8_t firmware_paddle_bits)
{
    // Reserved full-report bits are the custom controller-firmware transport,
    // so consume them independently of name/OUI/SDP timing. Genuine Switch
    // controllers leave them clear.
    buttons = switch_pro_translate_reserved_paddles(
        buttons, firmware_paddle_bits);

    // The stock-profile chord fallback is controller-specific and remains
    // restricted to the captured 8BitDo identity.
    if (device &&
        switch_pro_8bitdo_ultimate_match(device->bd_addr, device->name,
                                         device->vendor_id,
                                         device->product_id)) {
        return switch_pro_8bitdo_ultimate_translate_paddles(
            buttons, 0);
    }
    return buttons;
}

static input_joycon_side_t switch_joycon_side(const bthid_device_t *device)
{
    if (!device) return INPUT_JOYCON_SIDE_NONE;
    if (device->vendor_id == 0x057E) {
        if (device->product_id == 0x2006) return INPUT_JOYCON_SIDE_LEFT;
        if (device->product_id == 0x2007) return INPUT_JOYCON_SIDE_RIGHT;
    }
    if (device->name) {
        if (strstr(device->name, "Joy-Con (L)") != NULL) return INPUT_JOYCON_SIDE_LEFT;
        if (strstr(device->name, "Joy-Con (R)") != NULL) return INPUT_JOYCON_SIDE_RIGHT;
    }
    return INPUT_JOYCON_SIDE_NONE;
}

static bool switch_quarantine_pro_wake_input(const bthid_device_t *device,
                                             const switch_bt_data_t *sw)
{
    if (!device || !sw || sw->init_state == SWITCH_STATE_ACTIVE) {
        return false;
    }

    // The genuine Switch 1 Pro emits temporary simple-mode input while the
    // driver negotiates report 0x30. On a dock sleep power-cycle, that startup
    // stream can contain a neutral report followed by synthetic/restored
    // button state, which looks like a real neutral->pressed wake edge.
    //
    // Continue routing those reports to USB, but exclude them from automatic
    // wake until all initialization subcommands have settled. The known
    // first-generation 8BitDo Ultimate path already has confirmed reconnect/
    // wake behavior and deliberately remains unchanged.
    if (switch_pro_8bitdo_ultimate_match(device->bd_addr, device->name,
                                         device->vendor_id,
                                         device->product_id)) {
        return false;
    }

    const bool pro_name =
        device->name && strstr(device->name, "Pro Controller") != NULL;
    const bool pro_pid =
        device->vendor_id == 0x057E && device->product_id == 0x2009;
    return pro_name || pro_pid;
}

// Encode rumble intensity to Switch rumble format (from USB Switch Pro driver)
// Each motor uses 4 bytes: [amplitude, HF_freq, amplitude/2, LF_freq]
// Neutral state: [00 01 40 40]
//
// This is a bounded linear approximation, not Nintendo's logarithmic HD-rumble curve.
// Fidelity and safe-range research is tracked in docs/bluetooth/output-open-questions.md.
static void encode_rumble(uint8_t intensity, uint8_t* out)
{
    if (intensity == 0) {
        out[0] = 0x00;
        out[1] = 0x01;
        out[2] = 0x40;
        out[3] = 0x40;
        return;
    }
    uint8_t amplitude = (uint8_t)(((uint16_t)intensity * 102) / 255 + 64);
    out[0] = amplitude;
    out[1] = 0x88;
    out[2] = amplitude / 2;
    out[3] = 0x61;
}

static void switch_send_subcommand(bthid_device_t* device, uint8_t subcmd,
                                    const uint8_t* data, uint8_t len)
{
    switch_bt_data_t* sw = (switch_bt_data_t*)device->driver_data;
    if (!sw) return;

    uint8_t buf[48];
    memset(buf, 0, sizeof(buf));

    buf[0] = sw->output_seq++ & 0x0F;

    // Neutral rumble data (8 bytes)
    buf[1] = 0x00; buf[2] = 0x01; buf[3] = 0x40; buf[4] = 0x40;
    buf[5] = 0x00; buf[6] = 0x01; buf[7] = 0x40; buf[8] = 0x40;

    buf[9] = subcmd;
    if (data && len > 0 && len < 38) {
        memcpy(&buf[10], data, len);
    }

    bthid_send_output_report(device->conn_index, SWITCH_REPORT_OUTPUT, buf, 10 + len);
}

static void switch_send_rumble(bthid_device_t* device, uint8_t left, uint8_t right)
{
    switch_bt_data_t* sw = (switch_bt_data_t*)device->driver_data;
    if (!sw) return;

    uint8_t buf[9];
    buf[0] = sw->output_seq++ & 0x0F;
    encode_rumble(left, &buf[1]);
    encode_rumble(right, &buf[5]);

    bthid_send_output_report(device->conn_index, SWITCH_REPORT_RUMBLE_ONLY, buf, 9);
}

// ============================================================================
// DRIVER IMPLEMENTATION
// ============================================================================

static bool switch_match(const char* device_name, const uint8_t* class_of_device,
                         uint16_t vendor_id, uint16_t product_id, bool is_ble)
{
    (void)class_of_device;
    (void)is_ble;

    // Match Switch 1 controllers by VID/PID
    // Nintendo VID = 0x057E
    // Switch 1 PIDs: Joy-Con L = 0x2006, Joy-Con R = 0x2007, Pro Controller = 0x2009
    // Do NOT match Switch 2 PIDs (0x2066, 0x2067, 0x2069, 0x2073) - handled by switch2_ble
    if (vendor_id == 0x057E) {
        switch (product_id) {
            case 0x2006:  // Joy-Con L
            case 0x2007:  // Joy-Con R
            case 0x2009:  // Pro Controller
                return true;
        }
        // Don't return true for unknown Nintendo PIDs
        // Let specific drivers handle them
    }

    // Name-based match (fallback for classic BT where VID/PID may be unavailable)
    if (device_name) {
        if (strstr(device_name, "Pro Controller") != NULL) {
            return true;
        }
        if (strstr(device_name, "Joy-Con") != NULL) {
            return true;
        }
    }

    return false;
}

static bool switch_init(bthid_device_t* device)
{
    printf("[SWITCH_BT] Init for device: %s\n", device->name);

    for (int i = 0; i < BTHID_MAX_DEVICES; i++) {
        if (!switch_data[i].initialized) {
            init_input_event(&switch_data[i].event);
            switch_data[i].initialized = true;
            switch_data[i].full_report_mode = false;
            switch_data[i].output_seq = 0;
            switch_data[i].rumble_left = 0;
            switch_data[i].rumble_right = 0;

            // Start init state machine — commands sent from task()
            switch_data[i].init_state = SWITCH_STATE_WAIT_READY;
            switch_data[i].init_time = platform_time_ms();

            switch_data[i].event.type = INPUT_TYPE_GAMEPAD;
            switch_data[i].event.transport = INPUT_TRANSPORT_BT_CLASSIC;
            switch_data[i].event.dev_addr = device->conn_index;
            switch_data[i].event.instance = 0;
            switch_data[i].event.button_count = 10;
            switch_data[i].event.joycon_side = switch_joycon_side(device);

            device->driver_data = &switch_data[i];
            return true;
        }
    }

    return false;
}

// Request `len` bytes from SPI flash at `addr` (subcommand 0x10). The reply
// comes back in report 0x21 and is parsed by switch_parse_spi_reply().
static void switch_spi_read(bthid_device_t* device, uint32_t addr, uint8_t len)
{
    uint8_t args[5] = {
        (uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF),
        (uint8_t)((addr >> 16) & 0xFF), (uint8_t)((addr >> 24) & 0xFF),
        len
    };
    switch_send_subcommand(device, SWITCH_SUBCMD_SPI_READ, args, sizeof(args));
}

// ============================================================================
// MOTION (docs/bluetooth/switch1-motion.md)
// ============================================================================
//
// Report 0x30 carries THREE 12-byte IMU frames at bytes 13-48, sampled 5 ms
// apart, each accel[XYZ] then gyro[XYZ] as int16 LE (§4/§5). The three frames
// together span the report interval, so their mean is the correct
// representative rate for integrating angular phase across that interval --
// the encoder downstream integrates over real elapsed time. Taking only the
// newest frame would throw away two thirds of the samples.
#define SW1_IMU_OFFSET      13
#define SW1_IMU_FRAME_SIZE  12
#define SW1_IMU_FRAMES      3

// Nominal scales (§6). Switch-1 gyro is ±2000 dps over int16, which is
// numerically IDENTICAL to the shared interchange convention that ds3_bt.c
// states ("±32767 = ±2000 dps"), so gyro passes through unscaled. Accel is
// ±8 g (~4096 counts/g) while the convention is ±4 g (8192 counts/g), so accel
// is doubled and clamped. SPI calibration (§7) supersedes both; until that read
// lands these are the documented fallback, and the encoder's stillness-gated
// bias tracker absorbs the gyro zero-rate offset.
#define SW1_ACCEL_TO_SINPUT 2

// Gyro sensitivity. §6 records a genuine disagreement: Nintendo's nominal maps
// ±2000 dps onto the full int16 (0.06103 dps/count, i.e. exactly the 16.384
// counts/dps interchange scale, so raw would pass through unscaled), while the
// LSM6DS3 datasheet gives 0.070 dps/count. The datasheet value is the one that
// matches hardware: a typical factory-calibration block yields
// 936/(sensitivity-origin) ≈ 936/13371 ≈ 0.070, and on hardware the nominal
// assumption under-reported rate — the controller needed noticeably more
// movement than a DualSense for the same on-screen result. Convert to the
// interchange scale with raw × 0.070 × 16.384 ≈ raw × 1.147.
#define SW1_GYRO_SCALE_NUM 1147
#define SW1_GYRO_SCALE_DEN 1000

// ---- SPI-flash factory/user calibration (§7) --------------------------------
// Subcommand 0x10 reads the controller's internal flash. The reply arrives in
// report 0x21 as: [13]=ACK, [14]=echoed subcommand, [15..18]=address (u32 LE),
// [19]=length, [20..]=data. (SWITCH_SUBCMD_SPI_READ is declared with the other
// subcommand ids at the top of the file.)
#define SW1_SPI_FACTORY_IMU_ADDR    0x6020u   // 24 B: accel origin/sens, gyro origin/sens
#define SW1_SPI_USER_IMU_ADDR       0x8026u   // 2 B magic (B2 A1) + the same 24 B
#define SW1_SPI_IMU_CAL_LEN         24u
#define SW1_SPI_USER_IMU_LEN        26u

// Reference constants from the factory-cal conditions (§7.4). Note the
// asymmetry: gyro subtracts its origin (a zero-rate offset) then scales, while
// accel scales about its origin/sensitivity span.
//   acc_g    = raw * 4.0 / (acc_sens - acc_origin)
//   gyro_dps = (raw - gyro_origin) * 936.0 / (gyro_sens - gyro_origin)
// Folded into the interchange scale (accel 8192/g, gyro 16.384 counts/dps) so
// the hot path stays integer-only:
//   accel_interchange = raw * 32768 / (acc_sens - acc_origin)
//   gyro_interchange  = (raw - gyro_origin) * 15335 / (gyro_sens - gyro_origin)
#define SW1_ACCEL_CAL_NUM 32768   // 4.0 g-span * 8192 counts/g
#define SW1_GYRO_CAL_NUM  15335   // 936 dps * 16.384 counts/dps

// Per-device axis signs. These describe a physical SENSOR REMOUNT, so they apply
// to the accelerometer and the gyroscope IDENTICALLY -- see the publish loop,
// which is written so a sign cannot reach one without reaching the other.
//
// Why that matters, and why it is not merely tidiness: the console fuses gyro
// with gravity to correct attitude. Gravity constrains PITCH and ROLL but
// carries no YAW information at all (rotating about the gravity vector does not
// change the measured vector). If accel and gyro are handed over in mutually
// mirrored frames, yaw still looks perfect while pitch/roll drift for a second
// or two and then snap as the correction loses its argument with the gyro, and
// their steady state follows gravity rather than the (negated) gyro. That exact
// failure was observed on hardware on 2026-07-27, and ns2_seam.c records the
// same class of bug from the DualSense work: an earlier mapping there "made the
// console's gravity correction bleed one axis into another".
//
// DETERMINANT INVARIANT: a remount is a rotation, never a reflection, so the
// signed permutation must have determinant +1. The index permutation used below
// is cyclic (permutation sign +1), so the determinant reduces to
// pitch * yaw * roll -- signs therefore flip in PAIRS, never singly. A lone flip
// is not a physically realizable orientation and reintroduces exactly the
// mirrored-frame bug above. The _Static_assert below machine-checks this so the
// next person to "just invert the axis that looks wrong" gets a build error
// instead of a subtle drift-and-snap on hardware.
//
// §8 is explicit that the two Joy-Con halves mount the IMU mirrored and that the
// Pro differs again, so a single table cannot serve all three and the values
// must come from observation rather than memory.
//
// Pro Controller (0x2009): pitch inverted — confirmed on hardware (vertical aim
// reversed), yaw confirmed correct. Roll is then forced to -1 by the determinant
// invariant rather than measured independently; it was wrong before this and had
// simply not been exercised, roll being the least-used aiming axis.
// Joy-Con L (0x2006) / R (0x2007): NOT yet observed. They inherit the Pro's
// signs as a starting point; correct them here once each half is tested.
#define SW1_PRO_SIGN_PITCH (-1)
#define SW1_PRO_SIGN_YAW   (+1)
#define SW1_PRO_SIGN_ROLL  (-1)
_Static_assert(SW1_PRO_SIGN_PITCH * SW1_PRO_SIGN_YAW * SW1_PRO_SIGN_ROLL == 1,
               "Switch 1 axis signs must form a proper rotation (determinant "
               "+1): accel and gyro share one frame, so signs flip in pairs");

typedef struct { int8_t pitch, yaw, roll; } sw1_axis_signs_t;

static sw1_axis_signs_t sw1_axis_signs_for(uint16_t product_id)
{
    switch (product_id) {
        case 0x2006:  // Joy-Con (L)   — unverified, see above
        case 0x2007:  // Joy-Con (R)   — unverified, see above
        case 0x2009:  // Pro Controller — pitch/yaw verified on hardware
        default:
            return (sw1_axis_signs_t){ .pitch = SW1_PRO_SIGN_PITCH,
                                       .yaw   = SW1_PRO_SIGN_YAW,
                                       .roll  = SW1_PRO_SIGN_ROLL };
    }
}

static int16_t sw1_clamp16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32767) return -32767;
    return (int16_t)v;
}

static int16_t sw1_rd16(const uint8_t* p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Decode a 24-byte IMU calibration payload: four groups of three int16 LE, in
// the order accel origin, accel sensitivity, gyro origin, gyro sensitivity
// (§7.1). The same layout serves the factory and user blocks.
static bool sw1_parse_imu_cal(const uint8_t* p, sw1_imu_cal_t* cal)
{
    for (int i = 0; i < 3; i++) {
        cal->accel_origin[i] = sw1_rd16(p +  0 + i * 2);
        cal->accel_sens[i]   = sw1_rd16(p +  6 + i * 2);
        cal->gyro_origin[i]  = sw1_rd16(p + 12 + i * 2);
        cal->gyro_sens[i]    = sw1_rd16(p + 18 + i * 2);
    }
    // A span of zero would divide by zero and an all-0xFF block is erased
    // flash, not calibration; reject both so the nominal fallback is used.
    for (int i = 0; i < 3; i++) {
        if (cal->accel_sens[i] - cal->accel_origin[i] == 0) return false;
        if (cal->gyro_sens[i]  - cal->gyro_origin[i]  == 0) return false;
    }
    cal->valid = true;
    return true;
}

// Handle a subcommand reply (report 0x21). Layout: [13]=ACK, [14]=echoed
// subcommand, [15..18]=address u32 LE, [19]=length, [20..]=data.
static void switch_parse_spi_reply(switch_bt_data_t* sw, const uint8_t* data,
                                   uint16_t len)
{
    if (len < 21 || data[14] != SWITCH_SUBCMD_SPI_READ) return;
    const uint32_t addr = (uint32_t)data[15] | ((uint32_t)data[16] << 8) |
                          ((uint32_t)data[17] << 16) | ((uint32_t)data[18] << 24);
    const uint8_t  n    = data[19];
    const uint8_t* body = &data[20];
    if (len < 20u + n) return;

    if (addr == SW1_SPI_FACTORY_IMU_ADDR && n >= SW1_SPI_IMU_CAL_LEN) {
        if (sw1_parse_imu_cal(body, &sw->imu_cal))
            printf("[SWITCH_BT] IMU factory calibration loaded\n");
    } else if (addr == SW1_SPI_USER_IMU_ADDR && n >= SW1_SPI_USER_IMU_LEN) {
        // Magic B2 A1 means a user calibration is present and takes precedence
        // over the factory block (§7.2); otherwise keep what we already have.
        if (body[0] == 0xB2 && body[1] == 0xA1) {
            sw1_imu_cal_t user;
            if (sw1_parse_imu_cal(body + 2, &user)) {
                sw->imu_cal = user;
                printf("[SWITCH_BT] IMU user calibration loaded (overrides factory)\n");
            }
        }
    }
}

// Mean of the three frames, per axis, in raw counts.
static void sw1_average_imu(const uint8_t* data, int32_t accel[3], int32_t gyro[3])
{
    for (int i = 0; i < 3; i++) { accel[i] = 0; gyro[i] = 0; }
    for (int f = 0; f < SW1_IMU_FRAMES; f++) {
        const uint8_t* fr = data + SW1_IMU_OFFSET + f * SW1_IMU_FRAME_SIZE;
        for (int i = 0; i < 3; i++) {
            accel[i] += sw1_rd16(fr + i * 2);
            gyro[i]  += sw1_rd16(fr + 6 + i * 2);
        }
    }
    for (int i = 0; i < 3; i++) {
        accel[i] /= SW1_IMU_FRAMES;
        gyro[i]  /= SW1_IMU_FRAMES;
    }
}

static void switch_process_report(bthid_device_t* device, const uint8_t* data, uint16_t len)
{
    switch_bt_data_t* sw = (switch_bt_data_t*)device->driver_data;
    if (!sw || len < 1) return;

    uint8_t report_id = data[0];

    // Subcommand replies (0x21) carry the SPI-flash calibration we requested
    // during init. The report also repeats buttons/sticks, but those are already
    // served by 0x30 once full report mode is active, so only the reply payload
    // is consumed here.
    if (report_id == SWITCH_REPORT_SUBCMD_REPLY) {
        switch_parse_spi_reply(sw, data, len);
        return;
    }

    if (report_id == SWITCH_REPORT_INPUT_STANDARD && len >= 13) {
        // Full input report (0x30)
        const switch_input_report_t* rpt = (const switch_input_report_t*)data;

        sw->full_report_mode = true;

        // Build button state
        uint32_t buttons = 0x00000000;

        // Face buttons (map by position, not label — Nintendo layout is rotated)
        if (rpt->b)      buttons |= JP_BUTTON_B1;  // B = bottom
        if (rpt->a)      buttons |= JP_BUTTON_B2;  // A = right
        if (rpt->y)      buttons |= JP_BUTTON_B3;  // Y = left
        if (rpt->x)      buttons |= JP_BUTTON_B4;  // X = top

        // Shoulder buttons
        if (rpt->l)      buttons |= JP_BUTTON_L1;
        if (rpt->r)      buttons |= JP_BUTTON_R1;
        if (rpt->zl)     buttons |= JP_BUTTON_L2;
        if (rpt->zr)     buttons |= JP_BUTTON_R2;

        // Full report 0x30 preserves the two rail buttons separately from the
        // top shoulder. Simple report 0x3F has no SL/SR fields.
        sw->event.joycon_side = switch_joycon_side(device);
        if (sw->event.joycon_side == INPUT_JOYCON_SIDE_LEFT) {
            if (rpt->sl_l) buttons |= JP_BUTTON_SL;
            if (rpt->sr_l) buttons |= JP_BUTTON_SR;
        } else if (sw->event.joycon_side == INPUT_JOYCON_SIDE_RIGHT) {
            if (rpt->sl_r) buttons |= JP_BUTTON_SL;
            if (rpt->sr_r) buttons |= JP_BUTTON_SR;
        }

        // System buttons
        if (rpt->minus)  buttons |= JP_BUTTON_S1;
        if (rpt->plus)   buttons |= JP_BUTTON_S2;
        if (rpt->lstick) buttons |= JP_BUTTON_L3;
        if (rpt->rstick) buttons |= JP_BUTTON_R3;
        if (rpt->home)   buttons |= JP_BUTTON_A1;
        if (rpt->capture) buttons |= JP_BUTTON_A2;

        // D-pad
        if (rpt->up)     buttons |= JP_BUTTON_DU;
        if (rpt->down)   buttons |= JP_BUTTON_DD;
        if (rpt->left)   buttons |= JP_BUTTON_DL;
        if (rpt->right)  buttons |= JP_BUTTON_DR;

        // Read the reserved extension bits from the wire byte explicitly.
        // This avoids implementation-defined C bitfield packing.
        const uint8_t firmware_paddle_bits =
            switch_pro_extract_reserved_paddles(data, len);
        sw->event.buttons = switch_translate_vendor_paddles(
            device, buttons, firmware_paddle_bits);

        // Unpack 12-bit sticks
        uint16_t lx = unpack_stick_12bit(rpt->left_stick, false);
        uint16_t ly = unpack_stick_12bit(rpt->left_stick, true);
        uint16_t rx = unpack_stick_12bit(rpt->right_stick, false);
        uint16_t ry = unpack_stick_12bit(rpt->right_stick, true);

        // Scale to 8-bit and invert Y (Nintendo: up=high, HID: up=low)
        sw->event.analog[ANALOG_LX] = scale_12bit_to_8bit(lx);
        sw->event.analog[ANALOG_LY] = 255 - scale_12bit_to_8bit(ly);
        sw->event.analog[ANALOG_RX] = scale_12bit_to_8bit(rx);
        sw->event.analog[ANALOG_RY] = 255 - scale_12bit_to_8bit(ry);

        controller_battery_t battery;
        if (controller_battery_decode_switch_pro(rpt->battery_conn, &battery)) {
            input_event_set_native_battery(&sw->event, battery.level,
                                           battery.charging);
        }

        // --- Motion -------------------------------------------------------
        // Published in the DualSense slot convention, because ns2_seam.c
        // remounts that frame into the Pro2 frame for every source.
        //
        // Switch-1 gyro axes are X=roll, Y=pitch, Z=yaw (§8) and the
        // accelerometer shares those body axes. The DualSense slots are
        // gyro[pitch, yaw, roll] and accel[X=lateral, Y=up, Z=forward]
        // (dualsense-motion.md §5), which gives the index remount below.
        //
        // Slot i of BOTH arrays refers to the same body axis: gyro[i] is the
        // rotation about the axis accel[i] measures. The remount and the signs
        // must therefore be applied to accel and gyro identically -- see the
        // long note on sw1_axis_signs_for() for what breaks otherwise, and why
        // the signs must multiply to +1.
        if (len >= SW1_IMU_OFFSET + SW1_IMU_FRAMES * SW1_IMU_FRAME_SIZE) {
            int32_t a[3], g[3];
            sw1_average_imu(data, a, g);

            const sw1_axis_signs_t s = sw1_axis_signs_for(device->product_id);

            // Per-unit calibration when SPI flash gave us one, else the nominal
            // fallback. Calibrated form (§7.4) folded into the interchange
            // scale, integer-only:
            //   accel = raw * 32768 / (acc_sens - acc_origin)
            //   gyro  = (raw - gyro_origin) * 15335 / (gyro_sens - gyro_origin)
            int32_t ax[3], gx[3];
            if (sw->imu_cal.valid) {
                for (int i = 0; i < 3; i++) {
                    ax[i] = (a[i] * SW1_ACCEL_CAL_NUM) /
                            (sw->imu_cal.accel_sens[i] - sw->imu_cal.accel_origin[i]);
                    gx[i] = ((g[i] - sw->imu_cal.gyro_origin[i]) * SW1_GYRO_CAL_NUM) /
                            (sw->imu_cal.gyro_sens[i] - sw->imu_cal.gyro_origin[i]);
                }
            } else {
                for (int i = 0; i < 3; i++) {
                    ax[i] = a[i] * SW1_ACCEL_TO_SINPUT;
                    gx[i] = (g[i] * SW1_GYRO_SCALE_NUM) / SW1_GYRO_SCALE_DEN;
                }
            }

            // Slot -> source-axis remount. Switch 1 body axes are 0=roll,
            // 1=pitch, 2=yaw (§8); the DualSense slots this publishes into are
            // 0=lateral/pitch, 1=up/yaw, 2=forward/roll. Cyclic, so permutation
            // sign is +1 (see the determinant invariant above).
            static const uint8_t remount[3] = { 1, 2, 0 };
            const int32_t sign[3] = { s.pitch, s.yaw, s.roll };

            // One loop for both sensors: this is the structural guarantee that
            // accel and gyro can never again drift into different frames.
            for (int slot = 0; slot < 3; slot++) {
                const uint8_t src = remount[slot];
                sw->event.accel[slot] = sw1_clamp16(ax[src] * sign[slot]);
                sw->event.gyro[slot]  = sw1_clamp16(gx[src] * sign[slot]);
            }

            sw->event.gyro_range  = 2000;   // interchange full scale
            sw->event.accel_range = 8000;   // Switch-1 is ±8 g natively
            // Byte 1 of the report is a report counter, not a sensor clock, so
            // it is not published as a motion timestamp (§9).
            sw->event.motion_sequence++;
            sw->event.motion_timestamp_valid = false;
            sw->event.has_motion = true;
        }

        sw->event.suppress_wake_input =
            switch_quarantine_pro_wake_input(device, sw);
        router_submit_input(&sw->event);

    } else if (report_id == SWITCH_REPORT_INPUT_SIMPLE && len >= 12) {
        // Simple HID report (0x3F) - used before full mode enabled
        const switch_simple_report_t* rpt = (const switch_simple_report_t*)data;

        uint32_t buttons = 0x00000000;

        if (rpt->b)      buttons |= JP_BUTTON_B1;  // B = bottom
        if (rpt->a)      buttons |= JP_BUTTON_B2;  // A = right
        if (rpt->y)      buttons |= JP_BUTTON_B3;  // Y = left
        if (rpt->x)      buttons |= JP_BUTTON_B4;  // X = top
        if (rpt->l)      buttons |= JP_BUTTON_L1;
        if (rpt->r)      buttons |= JP_BUTTON_R1;
        if (rpt->zl)     buttons |= JP_BUTTON_L2;
        if (rpt->zr)     buttons |= JP_BUTTON_R2;
        if (rpt->minus)  buttons |= JP_BUTTON_S1;
        if (rpt->plus)   buttons |= JP_BUTTON_S2;
        if (rpt->lstick) buttons |= JP_BUTTON_L3;
        if (rpt->rstick) buttons |= JP_BUTTON_R3;
        if (rpt->home)   buttons |= JP_BUTTON_A1;
        if (rpt->capture) buttons |= JP_BUTTON_A2;

        // Hat to D-pad
        if (rpt->hat == 0 || rpt->hat == 1 || rpt->hat == 7) buttons |= JP_BUTTON_DU;
        if (rpt->hat >= 1 && rpt->hat <= 3) buttons |= JP_BUTTON_DR;
        if (rpt->hat >= 3 && rpt->hat <= 5) buttons |= JP_BUTTON_DD;
        if (rpt->hat >= 5 && rpt->hat <= 7) buttons |= JP_BUTTON_DL;

        // The controller firmware patch intentionally exposes paddles only in
        // normal full-report mode. Ignore simple-report reserved bits so the
        // stock firmware's transient bit 7 behavior cannot become a paddle.
        sw->event.buttons = switch_translate_vendor_paddles(device, buttons, 0);
        // 16-bit sticks scaled to 8-bit (0-65535 → 0-255)
        sw->event.analog[ANALOG_LX] = rpt->lx >> 8;
        sw->event.analog[ANALOG_LY] = 255 - (rpt->ly >> 8);  // Invert Y (Nintendo: up=high, HID: up=low)
        sw->event.analog[ANALOG_RX] = rpt->rx >> 8;
        sw->event.analog[ANALOG_RY] = 255 - (rpt->ry >> 8);  // Invert Y (Nintendo: up=high, HID: up=low)

        sw->event.suppress_wake_input =
            switch_quarantine_pro_wake_input(device, sw);
        router_submit_input(&sw->event);
    }
}

static void switch_task(bthid_device_t* device)
{
    switch_bt_data_t* sw = (switch_bt_data_t*)device->driver_data;
    if (!sw) return;

    uint32_t now = platform_time_ms();

    switch (sw->init_state) {
        case SWITCH_STATE_WAIT_READY:
            // Wait before sending first subcommand
            if (now - sw->init_time >= SWITCH_INIT_DELAY_MS) {
                printf("[SWITCH_BT] Sending set input mode (0x30)\n");
                uint8_t mode = SWITCH_INPUT_MODE_FULL;
                switch_send_subcommand(device, SWITCH_SUBCMD_SET_INPUT_MODE, &mode, 1);
                sw->init_state = SWITCH_STATE_SET_INPUT_MODE;
                sw->init_time = now;
            }
            break;

        case SWITCH_STATE_SET_INPUT_MODE:
            if (now - sw->init_time >= SWITCH_INIT_DELAY_MS) {
                // The 6-axis IMU ships DISABLED; until this is sent the 0x30
                // report's motion block stays zero (switch1-motion.md §3).
                // SWITCH_SUBCMD_ENABLE_IMU was already defined in this file but
                // never sent, which is precisely why Switch-1 motion was absent.
                printf("[SWITCH_BT] Sending enable IMU\n");
                uint8_t imu_on = 0x01;
                switch_send_subcommand(device, SWITCH_SUBCMD_ENABLE_IMU, &imu_on, 1);
                sw->init_state = SWITCH_STATE_ENABLE_IMU;
                sw->init_time = now;
            }
            break;

        case SWITCH_STATE_ENABLE_IMU:
            if (now - sw->init_time >= SWITCH_INIT_DELAY_MS) {
                // Factory IMU calibration. Accurate motion needs the per-unit
                // origin/sensitivity pairs; the nominal scales are only a
                // fallback (§7). The reply arrives in report 0x21.
                switch_spi_read(device, SW1_SPI_FACTORY_IMU_ADDR,
                                SW1_SPI_IMU_CAL_LEN);
                sw->init_state = SWITCH_STATE_READ_FACTORY_CAL;
                sw->init_time = now;
            }
            break;

        case SWITCH_STATE_READ_FACTORY_CAL:
            if (now - sw->init_time >= SWITCH_INIT_DELAY_MS) {
                // User calibration overrides factory when its magic is present.
                // Read magic + payload in one go (§7.2).
                switch_spi_read(device, SW1_SPI_USER_IMU_ADDR,
                                SW1_SPI_USER_IMU_LEN);
                sw->init_state = SWITCH_STATE_READ_USER_CAL;
                sw->init_time = now;
            }
            break;

        case SWITCH_STATE_READ_USER_CAL:
            if (now - sw->init_time >= SWITCH_INIT_DELAY_MS) {
                printf("[SWITCH_BT] Sending enable vibration\n");
                uint8_t enable = 0x01;
                switch_send_subcommand(device, SWITCH_SUBCMD_ENABLE_VIBRATION, &enable, 1);
                sw->init_state = SWITCH_STATE_ENABLE_VIBRATION;
                sw->init_time = now;
            }
            break;

        case SWITCH_STATE_ENABLE_VIBRATION:
            if (now - sw->init_time >= SWITCH_INIT_DELAY_MS) {
                // Set player LED based on player index
                int player_idx = find_player_index(sw->event.dev_addr, sw->event.instance);
                uint8_t player_num = (player_idx >= 0) ? player_idx + 1 : 1;
                uint8_t pattern = 0;
                if (player_num >= 1 && player_num <= 4) {
                    pattern = (1 << player_num) - 1;
                }
                printf("[SWITCH_BT] Sending player LED (player %d, pattern 0x%02X)\n",
                       player_num, pattern);
                switch_send_subcommand(device, SWITCH_SUBCMD_SET_PLAYER_LED, &pattern, 1);
                sw->init_state = SWITCH_STATE_SET_PLAYER_LED;
                sw->init_time = now;
            }
            break;

        case SWITCH_STATE_SET_PLAYER_LED:
            if (now - sw->init_time >= SWITCH_INIT_DELAY_MS) {
                printf("[SWITCH_BT] Init complete\n");
                sw->init_state = SWITCH_STATE_ACTIVE;
            }
            break;

        case SWITCH_STATE_ACTIVE: {
            // Monitor feedback system for rumble/LED updates
            int player_idx = find_player_index(sw->event.dev_addr, sw->event.instance);
            if (player_idx < 0) break;

            feedback_state_t* fb = feedback_get_state(player_idx);
            if (!fb) break;

            // Handle rumble updates
            if (fb->rumble_dirty) {
                uint8_t left = fb->rumble.left;
                uint8_t right = fb->rumble.right;
                if (left != sw->rumble_left || right != sw->rumble_right) {
                    switch_send_rumble(device, left, right);
                    sw->rumble_left = left;
                    sw->rumble_right = right;
                }
            }

            // Handle LED updates
            if (fb->led_dirty) {
                uint8_t pattern = fb->led.pattern;
                if (pattern != 0) {
                    printf("[SWITCH_BT] LED update: pattern=0x%02X\n", pattern);
                    switch_send_subcommand(device, SWITCH_SUBCMD_SET_PLAYER_LED, &pattern, 1);
                }
            }

            if (fb->rumble_dirty || fb->led_dirty) {
                feedback_clear_dirty(player_idx);
            }
            break;
        }
    }
}

static void switch_disconnect(bthid_device_t* device)
{
    printf("[SWITCH_BT] Disconnect: %s\n", device->name);

    switch_bt_data_t* sw = (switch_bt_data_t*)device->driver_data;
    if (sw) {
        // Clear router state first (sends zeroed input report)
        router_device_disconnected(sw->event.dev_addr, sw->event.instance);
        // Remove player assignment
        remove_players_by_address(sw->event.dev_addr, sw->event.instance);

        init_input_event(&sw->event);
        sw->initialized = false;
    }
}

// ============================================================================
// DRIVER STRUCT
// ============================================================================

const bthid_driver_t switch_pro_bt_driver = {
    .name = "Switch Pro",
    // Switch 1 hardware (Joy-Con/Pro Controller) is Classic-BT only — never BLE.
    // See bthid_transport_mask_t in bthid.h for why this matters here specifically:
    // this driver's name-based fallback ("Pro Controller" substring) would otherwise
    // risk matching a BLE-connecting Switch 2 Pro Controller before switch2_ble.c
    // gets a chance, since this driver is registered first.
    .transports = BTHID_TRANSPORT_CLASSIC,
    .match = switch_match,
    .init = switch_init,
    .process_report = switch_process_report,
    .task = switch_task,
    .disconnect = switch_disconnect,
};

void switch_pro_bt_register(void)
{
    bthid_register_driver(&switch_pro_bt_driver);
}
