// Nintendo Switch Pro Controller protocol emulation (USB / wired).
//
// Ported to C from the bmelanman/retro-pico-switch reference (MIT), itself
// derived from dekuNukem's Switch reverse-engineering notes. Each emulated
// interface runs an independent handshake + subcommand state machine:
//
//   console --(OUT endpoint)-->  switch_pro_receive()   [stores request]
//   console <--(IN endpoint)---  switch_pro_generate_report()  [builds reply]
//
// Report families handled:
//   0x80  USB handshake          -> 0x81 reply
//   0x01  rumble + subcommand     -> 0x21 reply (subcommand ack + data)
//   0x10  rumble only            -> (parsed for rumble; streams 0x30)
//   else / steady state          -> 0x30 standard full input report
//
// All reports are built directly into the 64-byte USB buffer.

#include "switch_pro.h"

#include <string.h>

#include "pico/stdlib.h"

#include "report.h"

// Output report ids (controller -> console)
#define RID_SUBCMD_REPLY 0x21
#define RID_FULL_INPUT 0x30
#define RID_USB_REPLY 0x81

// Input report ids (console -> controller)
#define RID_RUMBLE_SUBCMD 0x01
#define RID_RUMBLE_ONLY 0x10
#define RID_USB_CMD 0x80

// Subcommand ids (byte 10 of a 0x01 report)
typedef enum {
    SUBCMD_BLUETOOTH_PAIR = 0x01,
    SUBCMD_REQUEST_DEVICE_INFO = 0x02,
    SUBCMD_SET_MODE = 0x03,
    SUBCMD_TRIGGER_BUTTONS = 0x04,
    SUBCMD_SET_SHIPMENT = 0x08,
    SUBCMD_SPI_READ = 0x10,
    SUBCMD_SET_NFC_IR_CONFIG = 0x21,
    SUBCMD_SET_NFC_IR_STATE = 0x22,
    SUBCMD_SET_PLAYER = 0x30,
    SUBCMD_TOGGLE_IMU = 0x40,
    SUBCMD_IMU_SENSITIVITY = 0x41,
    SUBCMD_ENABLE_VIBRATION = 0x48,
} switch_subcommand_t;

// Battery (full) + connection (wired/powered) nibble.
#define CONN_INFO 0x81

static const uint8_t VIB_OPTS[4] = {0x0a, 0x0c, 0x0b, 0x09};

typedef struct {
    uint8_t request[64];  // last output report from the console (request[0] = id)
    bool has_request;     // a console command awaits exactly one reply
    uint8_t addr[6];      // fake controller Bluetooth address
    bool vibration_enabled;
    uint8_t vibration_idx;
    uint8_t vibration_report;
    bool imu_enabled;
    uint8_t player_number;
    uint8_t timer;
    uint32_t timestamp_ms;
} switch_pro_ctx_t;

static switch_pro_ctx_t ctx[SWITCH_PRO_MAX_CONTROLLERS];

void switch_pro_init(void) {
    memset(ctx, 0, sizeof(ctx));
    for (uint8_t i = 0; i < SWITCH_PRO_MAX_CONTROLLERS; i++) {
        switch_pro_ctx_t *c = &ctx[i];
        // Distinct, deterministic fake MAC per interface (only echoed in
        // device-info; not used for pairing since we are the USB device).
        uint8_t a[6] = {0x7c, 0xbb, 0x8a, 0x00, 0x00, i};
        memcpy(c->addr, a, 6);
        // Announce ourselves on the first IN poll (matches the reference).
        c->request[0] = RID_USB_CMD;
        c->request[1] = 0x01;
        c->has_request = true;
    }
}

static uint8_t advance_timer(switch_pro_ctx_t *c) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (c->timestamp_ms == 0) {
        c->timestamp_ms = now;
        return c->timer;
    }
    uint32_t delta = now - c->timestamp_ms;
    c->timer = (uint8_t)((c->timer + delta * 4) & 0xFF);
    c->timestamp_ms = now;
    return c->timer;
}

static inline void put_le16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

// Fill out[0..12]: id, timer, connection info, buttons, sticks, vibration.
static void build_input_core(uint8_t instance, switch_pro_ctx_t *c, uint8_t *out, uint8_t report_id) {
    switch_pro_input_t in;
    get_global_gamepad_input(instance, &in);

    out[0] = report_id;
    out[1] = advance_timer(c);
    out[2] = CONN_INFO;
    memcpy(out + 3, in.buttons, 3);
    memcpy(out + 6, in.left_stick, 3);
    memcpy(out + 9, in.right_stick, 3);
    out[12] = c->vibration_report;

    // 0x30 carries the 6-axis IMU block (3 samples) when the console enabled it.
    if (report_id == RID_FULL_INPUT && c->imu_enabled) {
        for (int s = 0; s < 3; s++) {
            uint8_t *p = out + 13 + s * 12;
            put_le16(p + 0, in.accel[0]);
            put_le16(p + 2, in.accel[1]);
            put_le16(p + 4, in.accel[2]);
            put_le16(p + 6, in.gyro[0]);
            put_le16(p + 8, in.gyro[1]);
            put_le16(p + 10, in.gyro[2]);
        }
    }
}

// SPI flash emulation: return plausible factory calibration / colour data.
static void build_spi_read(switch_pro_ctx_t *c, uint8_t *out) {
    uint8_t addr_bottom = c->request[11];
    uint8_t addr_top = c->request[12];
    uint8_t read_length = c->request[15];

    out[13] = 0x90;  // ACK (SPI read)
    out[14] = SUBCMD_SPI_READ;
    out[15] = addr_bottom;
    out[16] = addr_top;
    out[19] = read_length;

    // Stick device parameters (deadzone / range). Same for both sticks.
    static const uint8_t params[18] = {0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3,
                                       0xD4, 0x14, 0x54, 0x41, 0x15, 0x54,
                                       0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63};

    if (addr_top == 0x60 && addr_bottom == 0x00) {
        memset(out + 20, 0xFF, 16);  // no serial number
    } else if (addr_top == 0x60 && addr_bottom == 0x50) {
        memset(out + 20, 0x32, 3);  // body colour
        memset(out + 23, 0xFF, 3);  // button colour
        memset(out + 26, 0xFF, 7);  // grip colours
    } else if (addr_top == 0x60 && addr_bottom == 0x80) {
        // Six-axis horizontal offsets: zeroed (we feed pre-referenced accel).
        memset(out + 20, 0x00, 6);
        memcpy(out + 26, params, sizeof(params));
    } else if (addr_top == 0x60 && addr_bottom == 0x98) {
        memcpy(out + 20, params, sizeof(params));
    } else if (addr_top == 0x80 && addr_bottom == 0x10) {
        memset(out + 20, 0xFF, 3);  // empty user stick calibration
    } else if (addr_top == 0x60 && addr_bottom == 0x3D) {
        static const uint8_t l_cal[9] = {0xD4, 0x75, 0x61, 0xE5, 0x87, 0x7C, 0xEC, 0x55, 0x61};
        static const uint8_t r_cal[9] = {0x5D, 0xD8, 0x7F, 0x18, 0xE6, 0x61, 0x86, 0x65, 0x5D};
        memcpy(out + 20, l_cal, sizeof(l_cal));
        memcpy(out + 29, r_cal, sizeof(r_cal));
        out[38] = 0xFF;
        memset(out + 39, 0x32, 3);  // body colour
        memset(out + 42, 0xFF, 3);  // button colour
    } else if (addr_top == 0x60 && addr_bottom == 0x20) {
        // Six-axis (IMU) factory calibration.
        //   bytes 0-5:   accelerometer origin   (zeroed: we send pre-referenced raw)
        //   bytes 6-11:  accelerometer sensitivity (0x4000 -> 4096 LSB/G)
        //   bytes 12-17: gyroscope origin       (zeroed: a still pad must read 0 rate,
        //                                         otherwise the console computes
        //                                         raw - origin = constant drift)
        //   bytes 18-23: gyroscope sensitivity  (0x343b, standard)
        static const uint8_t sa_cal[24] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
                                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x3b, 0x34, 0x3b, 0x34, 0x3b, 0x34};
        memcpy(out + 20, sa_cal, sizeof(sa_cal));
    } else {
        memset(out + 20, 0xFF, read_length);
    }
}

// Build a 0x21 subcommand reply for the subcommand in request[10].
// Returns true if it produced a reply; false if the subcommand is unknown
// (caller should fall back to a plain 0x30 input report).
static bool build_subcommand_reply(uint8_t instance, switch_pro_ctx_t *c, uint8_t *out) {
    uint8_t subcmd = c->request[10];

    // The vibration byte changes on each subcommand reply once enabled.
    if (c->vibration_enabled) {
        c->vibration_idx = (c->vibration_idx + 1) % 4;
        c->vibration_report = VIB_OPTS[c->vibration_idx];
    }

    build_input_core(instance, c, out, RID_SUBCMD_REPLY);

    switch (subcmd) {
        case SUBCMD_BLUETOOTH_PAIR:
            out[13] = 0x81;
            out[14] = 0x01;
            out[15] = 0x03;
            break;
        case SUBCMD_REQUEST_DEVICE_INFO:
            out[13] = 0x82;
            out[14] = 0x02;
            out[15] = 0x03;  // firmware version (hi)
            out[16] = 0x48;  // firmware version (lo)
            out[17] = 0x03;  // controller type: Pro Controller
            out[18] = 0x02;  // unknown, always 2
            memcpy(out + 19, c->addr, 6);
            out[25] = 0x01;  // unknown, always 1
            out[26] = 0x01;  // colours read from SPI
            break;
        case SUBCMD_SET_SHIPMENT:
            out[13] = 0x80;
            out[14] = SUBCMD_SET_SHIPMENT;
            break;
        case SUBCMD_SPI_READ:
            build_spi_read(c, out);
            break;
        case SUBCMD_SET_MODE:
            out[13] = 0x80;
            out[14] = SUBCMD_SET_MODE;
            break;
        case SUBCMD_TRIGGER_BUTTONS:
            out[13] = 0x83;
            out[14] = SUBCMD_TRIGGER_BUTTONS;
            break;
        case SUBCMD_TOGGLE_IMU:
            c->imu_enabled = (c->request[11] == 0x01);
            out[13] = 0x80;
            out[14] = SUBCMD_TOGGLE_IMU;
            break;
        case SUBCMD_IMU_SENSITIVITY:
            out[13] = 0x80;
            out[14] = SUBCMD_IMU_SENSITIVITY;
            break;
        case SUBCMD_ENABLE_VIBRATION:
            c->vibration_enabled = true;
            c->vibration_idx = 0;
            c->vibration_report = VIB_OPTS[0];
            out[13] = 0x80;
            out[14] = SUBCMD_ENABLE_VIBRATION;
            break;
        case SUBCMD_SET_PLAYER: {
            uint8_t bf = c->request[11];
            if (bf == 0x01 || bf == 0x10)
                c->player_number = 1;
            else if (bf == 0x03 || bf == 0x30)
                c->player_number = 2;
            else if (bf == 0x07 || bf == 0x70)
                c->player_number = 3;
            else if (bf == 0x0F || bf == 0xF0)
                c->player_number = 4;
            out[13] = 0x80;
            out[14] = SUBCMD_SET_PLAYER;
            break;
        }
        case SUBCMD_SET_NFC_IR_STATE:
            out[13] = 0x80;
            out[14] = SUBCMD_SET_NFC_IR_STATE;
            break;
        case SUBCMD_SET_NFC_IR_CONFIG: {
            static const uint8_t nfc[8] = {0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01};
            out[13] = 0xA0;
            out[14] = SUBCMD_SET_NFC_IR_CONFIG;
            memcpy(out + 15, nfc, sizeof(nfc));
            out[48] = 0xC8;
            break;
        }
        default:
            return false;  // unknown subcommand: fall back to 0x30
    }
    return true;
}

// Build the 0x81 reply for a 0x80 USB handshake command.
static void build_usb_handshake(uint8_t instance, switch_pro_ctx_t *c, uint8_t *out) {
    out[0] = RID_USB_REPLY;
    out[1] = c->request[1];
    switch (c->request[1]) {
        case 0x01:  // request MAC / device type
            out[3] = 0x03;
            for (int i = 0; i < 6; i++)
                out[4 + i] = c->addr[5 - i];
            break;
        case 0x02:  // handshake
        case 0x03:  // set baud rate
            break;
        default:
            // 0x04 (force USB) / 0x05 (disable USB) / others: start streaming input.
            build_input_core(instance, c, out, RID_FULL_INPUT);
            break;
    }
}

// Decode an approximate rumble amplitude (0..255) from a console rumble report.
// The Switch's HD-rumble encoding is complex; we take the high-band amplitude of
// whichever side is active, which is enough to drive a simple vibration motor.
static uint8_t decode_rumble(const uint8_t *buf, uint16_t len) {
    if (len < 10)
        return 0;
    bool l_valid = (buf[2] & 0x03) == 0x00 && (buf[5] & 0x40) == 0x40;
    bool r_valid = (buf[6] & 0x03) == 0x00 && (buf[9] & 0x40) == 0x40;
    uint8_t hi = 0;
    if (l_valid && (buf[5] & 0x3F) > hi)
        hi = buf[5] & 0x3F;
    if (r_valid && (buf[9] & 0x3F) > hi)
        hi = buf[9] & 0x3F;
    uint16_t amp = (uint16_t)hi * 4;  // 0..63 -> 0..252
    return amp > 255 ? 255 : (uint8_t)amp;
}

void switch_pro_receive(uint8_t instance, const uint8_t *buf, uint16_t len) {
    if (instance >= SWITCH_PRO_MAX_CONTROLLERS || buf == NULL || len == 0)
        return;
    switch_pro_ctx_t *c = &ctx[instance];

    uint8_t id = buf[0];
    if (id == RID_RUMBLE_SUBCMD || id == RID_RUMBLE_ONLY || id == 0x11)
        report_set_rumble(instance, decode_rumble(buf, len));

    uint16_t n = len > sizeof(c->request) ? sizeof(c->request) : len;
    memset(c->request, 0, sizeof(c->request));
    memcpy(c->request, buf, n);
    c->has_request = true;
}

uint16_t switch_pro_generate_report(uint8_t instance, uint8_t *out) {
    if (instance >= SWITCH_PRO_MAX_CONTROLLERS || out == NULL)
        return 0;
    switch_pro_ctx_t *c = &ctx[instance];

    memset(out, 0, 64);

    if (c->has_request && c->request[0] == RID_USB_CMD) {
        build_usb_handshake(instance, c, out);
        c->has_request = false;
    } else if (c->has_request && c->request[0] == RID_RUMBLE_SUBCMD) {
        if (!build_subcommand_reply(instance, c, out))
            build_input_core(instance, c, out, RID_FULL_INPUT);
        c->has_request = false;
    } else {
        c->has_request = false;
        build_input_core(instance, c, out, RID_FULL_INPUT);
    }
    return 64;
}
