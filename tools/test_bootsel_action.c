#include <stdio.h>

#include "bootsel_action.h"

static int failures;

#define CHECK(condition, message)                                            \
    do {                                                                     \
        if (!(condition)) {                                                  \
            printf("FAIL: %s\n", message);                                 \
            failures++;                                                      \
        } else {                                                             \
            printf("OK:   %s\n", message);                                 \
        }                                                                    \
    } while (0)

int main(void)
{
    CHECK(bootsel_action_resolve(
              BOOTSEL_SINGLE_TAP, false, false) == BOOTSEL_ACTION_NONE,
          "unpaired single tap does nothing");
    CHECK(bootsel_action_resolve(
              BOOTSEL_SINGLE_TAP, false, true) ==
              BOOTSEL_ACTION_CYCLE_CONTROLLER,
          "paired single tap cycles controller personalities");
    CHECK(bootsel_action_resolve(
              BOOTSEL_SINGLE_TAP, true, true) == BOOTSEL_ACTION_NONE,
          "single tap does not cycle from Config");

    CHECK(bootsel_action_resolve(
              BOOTSEL_DOUBLE_TAP, false, false) ==
              BOOTSEL_ACTION_OPEN_PAIRING,
          "unpaired double tap opens pairing");
    CHECK(bootsel_action_resolve(
              BOOTSEL_DOUBLE_TAP, false, true) ==
              BOOTSEL_ACTION_OPEN_PAIRING,
          "paired double tap opens pairing");
    CHECK(bootsel_action_resolve(
              BOOTSEL_DOUBLE_TAP, true, true) == BOOTSEL_ACTION_NONE,
          "double tap is suppressed in Config");

    CHECK(bootsel_action_resolve(
              BOOTSEL_TRIPLE_TAP, false, false) ==
              BOOTSEL_ACTION_WIPE_DEVICES,
          "unpaired triple tap clears stored devices");
    CHECK(bootsel_action_resolve(
              BOOTSEL_TRIPLE_TAP, false, true) ==
              BOOTSEL_ACTION_WIPE_DEVICES,
          "paired triple tap clears and disconnects devices");
    CHECK(bootsel_action_resolve(
              BOOTSEL_TRIPLE_TAP, true, true) ==
              BOOTSEL_ACTION_WIPE_DEVICES,
          "triple tap remains an emergency wipe in Config");

    CHECK(bootsel_action_resolve(
              BOOTSEL_HOLD, false, false) ==
              BOOTSEL_ACTION_TOGGLE_CONFIG,
          "unpaired hold enters Config");
    CHECK(bootsel_action_resolve(
              BOOTSEL_HOLD, false, true) ==
              BOOTSEL_ACTION_TOGGLE_CONFIG,
          "paired hold enters Config");
    CHECK(bootsel_action_resolve(
              BOOTSEL_HOLD, true, true) ==
              BOOTSEL_ACTION_TOGGLE_CONFIG,
          "hold exits Config");

    puts(failures ? "bootsel_action: failures" :
                    "bootsel_action: all tests passed");
    return failures ? 1 : 0;
}
