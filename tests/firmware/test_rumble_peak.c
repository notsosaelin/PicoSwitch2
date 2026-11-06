#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "rumble_peak.h"

int main(void) {
    rumble_peak_t peak = {0, 0};

    // Five 4 ms Nintendo updates can occur inside one ~21.3 ms DualSense
    // audio packet. The last value is not necessarily the strongest.
    rumble_peak_push(&peak, 6, 2);
    rumble_peak_push(&peak, 24, 18);
    rumble_peak_push(&peak, 68, 51);
    rumble_peak_push(&peak, 34, 40);
    rumble_peak_push(&peak, 20, 12);
    rumble_peak_t out = rumble_peak_take(&peak, 20, 12);
    assert(out.left == 68);
    assert(out.right == 51);

    // After consumption, a sustained current command is the next baseline.
    out = rumble_peak_take(&peak, 20, 12);
    assert(out.left == 20);
    assert(out.right == 12);

    // A STOP remains a STOP once the prior interval has been consumed.
    rumble_peak_push(&peak, 0, 0);
    out = rumble_peak_take(&peak, 0, 0);
    assert(out.left == 20);
    assert(out.right == 12);
    out = rumble_peak_take(&peak, 0, 0);
    assert(out.left == 0);
    assert(out.right == 0);

    // Stereo peaks are independent.
    rumble_peak_push(&peak, 9, 80);
    rumble_peak_push(&peak, 70, 4);
    out = rumble_peak_take(&peak, 70, 4);
    assert(out.left == 70);
    assert(out.right == 80);

    // Starting a new audio session discards peaks accumulated while native
    // streaming was inactive, retaining only the live command.
    rumble_peak_reset(&peak, 3, 5);
    out = rumble_peak_take(&peak, 3, 5);
    assert(out.left == 3);
    assert(out.right == 5);

    puts("rumble_peak: all tests passed");
    return 0;
}
