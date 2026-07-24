#include "ns2_diag_input.h"

static uint32_t y_release_us;
static bool y_active;

void ns2_diag_input_reset(void)
{
    y_release_us = 0;
    y_active = false;
}

void ns2_diag_input_press_y(uint32_t now_us, uint32_t duration_us)
{
    y_release_us = now_us + duration_us;
    y_active = duration_us != 0;
}

bool ns2_diag_input_y_pressed(uint32_t now_us)
{
    if (!y_active)
        return false;
    // Signed subtraction preserves the comparison across the 32-bit timer
    // wrap as long as a diagnostic pulse is shorter than 2^31 microseconds.
    if ((int32_t)(y_release_us - now_us) <= 0) {
        y_active = false;
        return false;
    }
    return true;
}
