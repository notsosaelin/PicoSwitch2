#include "report.h"

#include <string.h>

#include "pico/critical_section.h"

#include "switch_pro.h"

#define INPUT_SLOTS SWITCH_PRO_MAX_CONTROLLERS

#define DEV_NAME_MAX 40

static switch_pro_input_t s_inputs[INPUT_SLOTS];
static uint32_t s_raw_buttons[INPUT_SLOTS];  // unified JP_BUTTON_* bitmap (config live-view)
static uint8_t s_rumble[INPUT_SLOTS];
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
        s_rumble[i] = 0;
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

void report_set_rumble(uint8_t idx, uint8_t amplitude) {
    if (idx >= INPUT_SLOTS)
        return;
    critical_section_enter_blocking(&s_lock);
    s_rumble[idx] = amplitude;
    critical_section_exit(&s_lock);
}

uint8_t report_get_rumble(uint8_t idx) {
    if (idx >= INPUT_SLOTS)
        return 0;
    critical_section_enter_blocking(&s_lock);
    uint8_t v = s_rumble[idx];
    critical_section_exit(&s_lock);
    return v;
}

bool report_any_button_pressed(void) {
    bool pressed = false;
    critical_section_enter_blocking(&s_lock);
    for (int i = 0; i < INPUT_SLOTS; i++) {
        if (s_inputs[i].buttons[0] | s_inputs[i].buttons[1] | s_inputs[i].buttons[2]) {
            pressed = true;
            break;
        }
    }
    critical_section_exit(&s_lock);
    return pressed;
}
