#include "report.h"

#include <string.h>

#include "pico/critical_section.h"

#include "switch_pro.h"
#include "rumble_peak.h"

#define INPUT_SLOTS SWITCH_PRO_MAX_CONTROLLERS

#define DEV_NAME_MAX 40

static switch_pro_input_t s_inputs[INPUT_SLOTS];
static uint32_t s_raw_buttons[INPUT_SLOTS];  // unified JP_BUTTON_* bitmap (config live-view)
static uint8_t s_rumble_left[INPUT_SLOTS];
static uint8_t s_rumble_right[INPUT_SLOTS];
static uint32_t s_rumble_generation[INPUT_SLOTS];  // see report_get_rumble_gen()'s own comment
static rumble_peak_t s_rumble_audio_peak[INPUT_SLOTS];
static uint8_t s_player_led_wire[INPUT_SLOTS];
static uint32_t s_player_led_generation[INPUT_SLOTS];
static char s_dev_name[INPUT_SLOTS][DEV_NAME_MAX];  // connected controller name (config live-view)
static uint16_t s_dev_vid[INPUT_SLOTS];
static uint16_t s_dev_pid[INPUT_SLOTS];
static uint8_t s_raw_report[INPUT_SLOTS][RAW_REPORT_BYTES];  // raw HID report (config debug view)
static uint16_t s_raw_report_len[INPUT_SLOTS];
static critical_section_t s_lock;

void report_init(void) {
    critical_section_init(&s_lock);

    // Default every slot to "connected but idle": centered sticks, no buttons,
    // no motion. Avoids sticks reading as min before a controller sends data.
    switch_pro_input_t neutral;
    memset(&neutral, 0, sizeof(neutral));
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, neutral.left_stick);
    switch_pro_pack_stick(SWITCH_STICK_MID, SWITCH_STICK_MID, neutral.right_stick);

    for (int i = 0; i < INPUT_SLOTS; i++) {
        s_inputs[i] = neutral;
        s_rumble_left[i] = 0;
        s_rumble_right[i] = 0;
        s_rumble_audio_peak[i] = (rumble_peak_t){0, 0};
        s_player_led_wire[i] = 0;
        s_player_led_generation[i] = 0;
        s_raw_buttons[i] = 0;
        s_dev_name[i][0] = '\0';
        s_dev_vid[i] = 0;
        s_dev_pid[i] = 0;
        s_raw_report_len[i] = 0;
    }
}

void set_global_gamepad_input(uint8_t idx, const switch_pro_input_t *in) {
    if (idx >= INPUT_SLOTS || in == NULL)
        return;
    critical_section_enter_blocking(&s_lock);
    s_inputs[idx] = *in;
    critical_section_exit(&s_lock);
}

void get_global_gamepad_input(uint8_t idx, switch_pro_input_t *out) {
    if (out == NULL)
        return;
    if (idx >= INPUT_SLOTS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    critical_section_enter_blocking(&s_lock);
    *out = s_inputs[idx];
    critical_section_exit(&s_lock);
}

static int16_t clamp_mouse_delta(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

static int8_t clamp_mouse_wheel(int16_t value) {
    if (value < -127) return -127;
    if (value > 127) return 127;
    return (int8_t)value;
}

void accumulate_global_mouse_input(uint8_t idx, const switch_pro_input_t *in) {
    if (idx >= INPUT_SLOTS || in == NULL) return;
    critical_section_enter_blocking(&s_lock);
    int16_t dx = clamp_mouse_delta((int32_t)s_inputs[idx].mouse_delta_x + in->mouse_delta_x);
    int16_t dy = clamp_mouse_delta((int32_t)s_inputs[idx].mouse_delta_y + in->mouse_delta_y);
    int8_t wheel = clamp_mouse_wheel((int16_t)s_inputs[idx].mouse_delta_wheel +
                                     in->mouse_delta_wheel);
    s_inputs[idx] = *in;
    s_inputs[idx].mouse_delta_x = dx;
    s_inputs[idx].mouse_delta_y = dy;
    s_inputs[idx].mouse_delta_wheel = wheel;
    critical_section_exit(&s_lock);
}

void take_global_gamepad_input(uint8_t idx, switch_pro_input_t *out) {
    if (out == NULL) return;
    if (idx >= INPUT_SLOTS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    critical_section_enter_blocking(&s_lock);
    *out = s_inputs[idx];
    s_inputs[idx].mouse_delta_x = 0;
    s_inputs[idx].mouse_delta_y = 0;
    s_inputs[idx].mouse_delta_wheel = 0;
    critical_section_exit(&s_lock);
}

void set_global_raw_buttons(uint8_t idx, uint32_t jp_buttons) {
    if (idx >= INPUT_SLOTS)
        return;
    critical_section_enter_blocking(&s_lock);
    s_raw_buttons[idx] = jp_buttons;
    critical_section_exit(&s_lock);
}

uint32_t get_global_raw_buttons(uint8_t idx) {
    if (idx >= INPUT_SLOTS)
        return 0;
    critical_section_enter_blocking(&s_lock);
    uint32_t v = s_raw_buttons[idx];
    critical_section_exit(&s_lock);
    return v;
}

void set_global_device(uint8_t idx, const char *name, uint16_t vid, uint16_t pid) {
    if (idx >= INPUT_SLOTS)
        return;
    critical_section_enter_blocking(&s_lock);
    if (name) {
        strncpy(s_dev_name[idx], name, DEV_NAME_MAX - 1);
        s_dev_name[idx][DEV_NAME_MAX - 1] = '\0';
    } else {
        s_dev_name[idx][0] = '\0';
    }
    s_dev_vid[idx] = vid;
    s_dev_pid[idx] = pid;
    critical_section_exit(&s_lock);
}

void set_global_raw_report(uint8_t idx, const uint8_t *data, uint16_t len) {
    if (idx >= INPUT_SLOTS || data == NULL)
        return;
    if (len > RAW_REPORT_BYTES)
        len = RAW_REPORT_BYTES;
    critical_section_enter_blocking(&s_lock);
    memcpy(s_raw_report[idx], data, len);
    s_raw_report_len[idx] = len;
    critical_section_exit(&s_lock);
}

uint16_t get_global_raw_report(uint8_t idx, uint8_t *out, uint16_t maxlen) {
    if (idx >= INPUT_SLOTS || out == NULL)
        return 0;
    critical_section_enter_blocking(&s_lock);
    uint16_t n = s_raw_report_len[idx];
    if (n > maxlen)
        n = maxlen;
    memcpy(out, s_raw_report[idx], n);
    critical_section_exit(&s_lock);
    return n;
}

void get_global_device(uint8_t idx, char *name_out, uint16_t name_len, uint16_t *vid, uint16_t *pid) {
    if (name_out && name_len)
        name_out[0] = '\0';
    if (vid)
        *vid = 0;
    if (pid)
        *pid = 0;
    if (idx >= INPUT_SLOTS)
        return;
    critical_section_enter_blocking(&s_lock);
    if (name_out && name_len) {
        strncpy(name_out, s_dev_name[idx], name_len - 1);
        name_out[name_len - 1] = '\0';
    }
    if (vid)
        *vid = s_dev_vid[idx];
    if (pid)
        *pid = s_dev_pid[idx];
    critical_section_exit(&s_lock);
}

void report_set_rumble(uint8_t idx, uint8_t left, uint8_t right) {
    if (idx >= INPUT_SLOTS)
        return;
    critical_section_enter_blocking(&s_lock);
    s_rumble_left[idx] = left;
    s_rumble_right[idx] = right;
    rumble_peak_push(&s_rumble_audio_peak[idx], left, right);
    s_rumble_generation[idx]++;  // unconditional -- every call is a distinct event, even if the
                                 // value happens to match what was already stored (see
                                 // report_get_rumble_gen()'s own comment for why this matters).
    critical_section_exit(&s_lock);
}

void report_take_rumble_audio_peak(uint8_t idx, uint8_t *left, uint8_t *right) {
    if (idx >= INPUT_SLOTS) {
        if (left) *left = 0;
        if (right) *right = 0;
        return;
    }
    critical_section_enter_blocking(&s_lock);
    rumble_peak_t const value =
        rumble_peak_take(&s_rumble_audio_peak[idx],
                         s_rumble_left[idx], s_rumble_right[idx]);
    critical_section_exit(&s_lock);
    if (left) *left = value.left;
    if (right) *right = value.right;
}

void report_reset_rumble_audio_peak(uint8_t idx) {
    if (idx >= INPUT_SLOTS) return;
    critical_section_enter_blocking(&s_lock);
    rumble_peak_reset(&s_rumble_audio_peak[idx],
                      s_rumble_left[idx], s_rumble_right[idx]);
    critical_section_exit(&s_lock);
}

void report_get_rumble(uint8_t idx, uint8_t *left, uint8_t *right) {
    if (idx >= INPUT_SLOTS) {
        if (left) *left = 0;
        if (right) *right = 0;
        return;
    }
    critical_section_enter_blocking(&s_lock);
    uint8_t l = s_rumble_left[idx], r = s_rumble_right[idx];
    critical_section_exit(&s_lock);
    if (left) *left = l;
    if (right) *right = r;
}

void report_get_rumble_gen(uint8_t idx, uint8_t *left, uint8_t *right, uint32_t *generation) {
    if (idx >= INPUT_SLOTS) {
        if (left) *left = 0;
        if (right) *right = 0;
        if (generation) *generation = 0;
        return;
    }
    critical_section_enter_blocking(&s_lock);
    uint8_t l = s_rumble_left[idx], r = s_rumble_right[idx];
    uint32_t g = s_rumble_generation[idx];
    critical_section_exit(&s_lock);
    if (left) *left = l;
    if (right) *right = r;
    if (generation) *generation = g;
}

void report_set_player_leds(uint8_t idx, uint8_t wire_mask) {
    if (idx >= INPUT_SLOTS) return;
    critical_section_enter_blocking(&s_lock);
    s_player_led_wire[idx] = wire_mask;
    s_player_led_generation[idx]++;
    critical_section_exit(&s_lock);
}

void report_get_player_leds(uint8_t idx, uint8_t *wire_mask, uint32_t *generation) {
    if (idx >= INPUT_SLOTS) {
        if (wire_mask) *wire_mask = 0;
        if (generation) *generation = 0;
        return;
    }
    critical_section_enter_blocking(&s_lock);
    uint8_t mask = s_player_led_wire[idx];
    uint32_t gen = s_player_led_generation[idx];
    critical_section_exit(&s_lock);
    if (wire_mask) *wire_mask = mask;
    if (generation) *generation = gen;
}

bool report_any_button_pressed(void) {
    bool pressed = false;
    critical_section_enter_blocking(&s_lock);
    for (int i = 0; i < INPUT_SLOTS; i++) {
        // gc_extra is a discrete digital semantic bitmask (native Z / L,R detent) -- a real
        // "pressed" signal like buttons[]. left_trigger/right_trigger are deliberately
        // excluded: they're continuous analog and there's no existing threshold policy for
        // treating trigger travel as a wake-worthy "button press" here. `extra` (C/GL/GR) was
        // already excluded before this change -- not touched, to avoid an unrelated behavior
        // change to existing wake semantics.
        if (s_inputs[i].buttons[0] | s_inputs[i].buttons[1] | s_inputs[i].buttons[2] |
            s_inputs[i].gc_extra) {
            pressed = true;
            break;
        }
    }
    critical_section_exit(&s_lock);
    return pressed;
}
