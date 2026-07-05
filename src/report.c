#include "report.h"

#include <string.h>

#include "pico/critical_section.h"

#include "sdkconfig.h"
#include "switch_pro.h"

// One slot per Bluetooth-connectable controller. The USB side only exposes the
// first SWITCH_PRO_MAX_CONTROLLERS of these, but we keep room for every device
// bluepad32 may connect so writes are always in-bounds.
#define INPUT_SLOTS CONFIG_BLUEPAD32_MAX_DEVICES

static switch_pro_input_t s_inputs[INPUT_SLOTS];
static uint32_t s_raw_buttons[INPUT_SLOTS];  // unified JP_BUTTON_* bitmap (config live-view)
static uint8_t s_rumble[INPUT_SLOTS];
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
