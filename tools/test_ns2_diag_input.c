#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ns2_diag_input.h"

int main(void)
{
    ns2_diag_input_reset();
    assert(!ns2_diag_input_y_pressed(1000));

    ns2_diag_input_press_y(1000, 120000);
    assert(ns2_diag_input_y_pressed(1000));
    assert(ns2_diag_input_y_pressed(120999));
    assert(!ns2_diag_input_y_pressed(121000));
    assert(!ns2_diag_input_y_pressed(121001));

    ns2_diag_input_press_y(UINT32_MAX - 49, 100);
    assert(ns2_diag_input_y_pressed(UINT32_MAX - 49));
    assert(ns2_diag_input_y_pressed(49));
    assert(!ns2_diag_input_y_pressed(50));

    ns2_diag_input_press_y(500, 0);
    assert(!ns2_diag_input_y_pressed(500));

    puts("ns2_diag_input: all tests passed");
    return 0;
}
