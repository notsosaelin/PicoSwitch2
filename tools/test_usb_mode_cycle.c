/*
 * Host-compilable test for the pure USB mode-cycle logic
 * (src/usb_mode_cycle.c). No pico-sdk/TinyUSB dependency -- build and run
 * directly with a host C compiler:
 *
 *   gcc -DNS2_PRO=1 -I include -o test_usb_mode_cycle \
 *       tools/test_usb_mode_cycle.c src/usb_mode_cycle.c
 *   ./test_usb_mode_cycle
 *
 * Exit code 0 = all assertions passed.
 *
 * Updated 2026-07-14: Joy-Con 2 split into two separate, always-available
 * personalities (USB_PERSONALITY_JOYCON2_L / _R) instead of one reserved
 * placeholder -- this file had gone stale after Joy-Con2's initial Stage B+C
 * landing (it still asserted the old "reserved, unavailable" placeholder
 * behavior) and was only caught while making this exact change.
 *
 * Updated again 2026-07-14: the historical all-personality helper wraps Config
 * to Pro2 instead of staying terminal.
 *
 * Updated 2026-07-25: production single-tap selection uses the separate
 * controller-only helper. It excludes Config; the two-second Config toggle is
 * direct policy in usb.c and bootsel_action.c.
 *
 * Covers the
 * "mode-cycle next-available logic" / "Config is terminal until reset" /
 * "default personality is always Pro2 after boot" items from NSO-GC.md's
 * required validation list. The remaining items on that list (NS2_PRO=OFF
 * hold stays direct-to-Config; no L3/R3 GameCube output destination;
 * ZL/C/Home/Capture remain real GameCube capabilities) are not pure-logic
 * properties this file can exercise -- see docs/switch2-gc/usb-personality.md's
 * validation section for how each of those was verified instead
 * (code-inspection for the unchanged Switch-1 branch;
 * tools/verify_gc_descriptors.py's structural checks for the
 * descriptor-content ones).
 */
#include <stdio.h>
#include <string.h>

#include "usb_mode_cycle.h"

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                      \
            printf("FAIL: %s\n", msg);                                      \
            failures++;                                                    \
        } else {                                                            \
            printf("OK:   %s\n", msg);                                     \
        }                                                                   \
    } while (0)

int main(void) {
    // "default personality is always Pro2 after boot/reset initialization":
    // g_usb_personality is a global initialized to USB_PERSONALITY_SWITCH2_PRO2
    // (usb.c); that initializer is only correct-by-construction if the enum's
    // first value really is Pro2 (value 0, so even a missed explicit
    // initializer would still zero-init to the right thing via BSS).
    CHECK(USB_PERSONALITY_SWITCH2_PRO2 == 0,
          "USB_PERSONALITY_SWITCH2_PRO2 is enum value 0 (correct boot default "
          "even under plain zero-initialization)");

    // Availability table. Joy-Con 2 L/R are real, always-available, individually-selectable
    // experimental/test personalities -- not reserved placeholders, and not a merged/paired mode
    // (see docs/switch2-joycon2/protocol.md "Why not simultaneous L+R").
    CHECK(usb_personality_available(USB_PERSONALITY_SWITCH2_PRO2) == true,
          "Switch2Pro2 is available");
    CHECK(usb_personality_available(USB_PERSONALITY_NSO_GAMECUBE) == true,
          "NSOGameCube is available");
    CHECK(usb_personality_available(USB_PERSONALITY_JOYCON2_L) == true,
          "JoyCon2 Left is available");
    CHECK(usb_personality_available(USB_PERSONALITY_JOYCON2_R) == true,
          "JoyCon2 Right is available");
    CHECK(usb_personality_available(USB_PERSONALITY_CDC_CONFIG) == true,
          "CDCConfig is available");

    // Cycle order: Pro2 -> GameCube -> Joy-Con2 Left -> Joy-Con2 Right -> Config. All five stops
    // are real personalities now; nothing is skipped.
    CHECK(usb_next_personality(USB_PERSONALITY_SWITCH2_PRO2) == USB_PERSONALITY_NSO_GAMECUBE,
          "next(Pro2) == NSOGameCube");
    CHECK(usb_next_personality(USB_PERSONALITY_NSO_GAMECUBE) == USB_PERSONALITY_JOYCON2_L,
          "next(NSOGameCube) == JoyCon2 Left");
    CHECK(usb_next_personality(USB_PERSONALITY_JOYCON2_L) == USB_PERSONALITY_JOYCON2_R,
          "next(JoyCon2 Left) == JoyCon2 Right");
    CHECK(usb_next_personality(USB_PERSONALITY_JOYCON2_R) == USB_PERSONALITY_CDC_CONFIG,
          "next(JoyCon2 Right) == CDCConfig");

    // "Config has a live exit back to Pro2" (2026-07-14, supersedes the original "terminal until
    // reset" design): calling next() from Config must wrap back to Pro2, not stay put.
    CHECK(usb_next_personality(USB_PERSONALITY_CDC_CONFIG) == USB_PERSONALITY_SWITCH2_PRO2,
          "next(CDCConfig) == Pro2 (historical all-personality helper wraps)");

    CHECK(usb_next_controller_personality(USB_PERSONALITY_SWITCH2_PRO2) ==
              USB_PERSONALITY_NSO_GAMECUBE,
          "controller cycle: Pro2 -> NSOGameCube");
    CHECK(usb_next_controller_personality(USB_PERSONALITY_NSO_GAMECUBE) ==
              USB_PERSONALITY_JOYCON2_L,
          "controller cycle: NSOGameCube -> JoyCon2 Left");
    CHECK(usb_next_controller_personality(USB_PERSONALITY_JOYCON2_L) ==
              USB_PERSONALITY_JOYCON2_R,
          "controller cycle: JoyCon2 Left -> JoyCon2 Right");
    CHECK(usb_next_controller_personality(USB_PERSONALITY_JOYCON2_R) ==
              USB_PERSONALITY_SWITCH2_PRO2,
          "controller cycle skips Config and wraps to Pro2");
    CHECK(usb_next_controller_personality(USB_PERSONALITY_CDC_CONFIG) ==
              USB_PERSONALITY_SWITCH2_PRO2,
          "controller cycle from Config safely selects Pro2");

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
