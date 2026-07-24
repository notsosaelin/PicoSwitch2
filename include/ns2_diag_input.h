#ifndef NS2_DIAG_INPUT_H
#define NS2_DIAG_INPUT_H

#include <stdbool.h>
#include <stdint.h>

// Core0-only diagnostic input overlay. It is applied at final Pro Controller 2
// report serialization, so it cannot overwrite Bluetooth input state or create
// a wake edge in the ordinary controller-input policy.
void ns2_diag_input_reset(void);
void ns2_diag_input_press_y(uint32_t now_us, uint32_t duration_us);
bool ns2_diag_input_y_pressed(uint32_t now_us);

#endif  // NS2_DIAG_INPUT_H
