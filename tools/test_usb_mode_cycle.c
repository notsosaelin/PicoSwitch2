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
 * Covers the "mode-cycle next-available logic" / "Joy-Con 2 placeholder is
 * skipped while unavailable" / "Config is terminal until reset" / "default
 * personality is always Pro2 after boot" items from NSO-GC.md's required
 * validation list. The remaining items on that list (NS2_PRO=OFF hold stays
 * direct-to-Config; no L3/R3 GameCube output destination; ZL/C/Home/Capture
 * remain real GameCube capabilities) are not pure-logic properties this file
 * can exercise -- see docs/switch2-gc/usb-personality.md's validation section
 * for how each of those was verified instead (code-inspection for the
 * unchanged Switch-1 branch; tools/verify_gc_descriptors.py's structural
 * checks for the descriptor-content ones).
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

    // Availability table.
    CHECK(usb_personality_available(USB_PERSONALITY_SWITCH2_PRO2) == true,
          "Switch2Pro2 is available");
    CHECK(usb_personality_available(USB_PERSONALITY_NSO_GAMECUBE) == true,
          "NSOGameCube is available");
    CHECK(usb_personality_available(USB_PERSONALITY_JOYCON2) == false,
          "JoyCon2 (reserved) is NOT available -- must never be enumerated");
    CHECK(usb_personality_available(USB_PERSONALITY_CDC_CONFIG) == true,
          "CDCConfig is available");

    // Cycle order: Pro2 -> GameCube -> Config, with JoyCon2 transparently
    // skipped (it sits between GameCube and Config in the enum but is
    // unavailable).
    CHECK(usb_next_personality(USB_PERSONALITY_SWITCH2_PRO2) == USB_PERSONALITY_NSO_GAMECUBE,
          "next(Pro2) == NSOGameCube");
    CHECK(usb_next_personality(USB_PERSONALITY_NSO_GAMECUBE) == USB_PERSONALITY_CDC_CONFIG,
          "next(NSOGameCube) == CDCConfig (JoyCon2 skipped)");

    // "Config is terminal until reset in this first cut": calling next()
    // again from Config must be a no-op (stay put), not advance/wrap.
    CHECK(usb_next_personality(USB_PERSONALITY_CDC_CONFIG) == USB_PERSONALITY_CDC_CONFIG,
          "next(CDCConfig) == CDCConfig (terminal, no wrap)");

    // Defensive: even if somehow asked to advance from the reserved,
    // unavailable JoyCon2 state, the walk must still skip forward to the
    // next real personality rather than getting stuck or returning JoyCon2
    // itself.
    CHECK(usb_next_personality(USB_PERSONALITY_JOYCON2) == USB_PERSONALITY_CDC_CONFIG,
          "next(JoyCon2) == CDCConfig (defensive: skips past a reserved current value too)");

    printf("\n%s\n", failures == 0 ? "All checks passed." : "One or more checks FAILED.");
    return failures == 0 ? 0 : 1;
}
