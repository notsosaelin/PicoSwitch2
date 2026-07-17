#include "switch_pro_8bitdo.h"

#include "core/buttons.h"

#include <string.h>

// BTstack stores bd_addr_t least-significant byte first.  bthid.c prints a
// human address as bd_addr[5]..bd_addr[0].  E4:17:D8 is assigned to
// 8BITDO TECHNOLOGY HK LIMITED in the IEEE MA-L registry and is the prefix
// captured from the tested controller. The per-device suffix is not retained.
static bool has_8bitdo_oui(const uint8_t bd_addr[6])
{
    return bd_addr &&
           bd_addr[5] == 0xE4 &&
           bd_addr[4] == 0x17 &&
           bd_addr[3] == 0xD8;
}

bool switch_pro_8bitdo_ultimate_match(const uint8_t bd_addr[6],
                                      const char *name,
                                      uint16_t vendor_id,
                                      uint16_t product_id)
{
    if (!has_8bitdo_oui(bd_addr) || !name ||
        strcmp(name, "Pro Controller") != 0) {
        return false;
    }

    // SDP identity can arrive after HID input starts.  Accept the exact spoofed
    // Nintendo identity or a still-provisional half/fully unknown identity,
    // but reject any contradictory resolved VID or PID.
    if (vendor_id != 0 && vendor_id != 0x057E) {
        return false;
    }
    if (product_id != 0 && product_id != 0x2009) {
        return false;
    }
    return true;
}

uint8_t switch_pro_extract_reserved_paddles(
    const uint8_t *report, uint16_t report_len)
{
    if (!report || report_len <= 4 || report[0] != 0x30) {
        return 0;
    }
    return (report[4] >> 6) & 0x03;
}

uint32_t switch_pro_translate_reserved_paddles(
    uint32_t buttons, uint8_t firmware_paddle_bits)
{
    // Hardware capture of the ARMv6-M-compatible custom firmware established:
    //   physical P1 -> full report data[4] bit 7 -> bit 1 here -> GL
    //   physical P2 -> full report data[4] bit 6 -> bit 0 here -> GR
    //
    // These are reserved Switch protocol bits. Decode the extension directly
    // instead of identity-gating it: stock controllers leave both bits clear,
    // and doing so avoids losing the extension to late/incomplete identity
    // metadata.
    if (firmware_paddle_bits & 0x01) {
        buttons |= JP_BUTTON_R4;
    }
    if (firmware_paddle_bits & 0x02) {
        buttons |= JP_BUTTON_L4;
    }
    return buttons;
}

uint32_t switch_pro_8bitdo_ultimate_translate_paddles(
    uint32_t buttons, uint8_t firmware_paddle_bits)
{
    buttons = switch_pro_translate_reserved_paddles(
        buttons, firmware_paddle_bits);

    // Captured 2026-07-16 in full Switch report mode (0x30), with Ultimate
    // Software physical profile 2 configured with internal mapping entry 18
    // set to A+B+X+Y and entry 19 set to L+R+ZL+ZR.  The live physical-button
    // capture established the label/order that matters here:
    //   physical P1 -> raw button bytes 40 10 C0
    //   physical P2 -> raw button bytes 0F 00 00
    //
    // This firmware/gamepad-mode combination emits P1 as raw button bytes
    // 40 10 C0 (R + Home + L + ZL), not the requested literal chord.  P2 emits
    // 0F 00 00 (all four face buttons).  Both held
    // together emit their exact union 4F 10 C0 and remain asserted for the
    // entire physical hold.  Consume the injected controls before routing so
    // neither fake buttons nor the injected Home press reach the console.
    const uint32_t p1_signature =
        JP_BUTTON_R1 | JP_BUTTON_A1 | JP_BUTTON_L1 | JP_BUTTON_L2;
    const uint32_t p2_signature =
        JP_BUTTON_B1 | JP_BUTTON_B2 | JP_BUTTON_B3 | JP_BUTTON_B4;

    if ((buttons & p1_signature) == p1_signature) {
        buttons &= ~p1_signature;
        buttons |= JP_BUTTON_L4;
    }
    if ((buttons & p2_signature) == p2_signature) {
        buttons &= ~p2_signature;
        buttons |= JP_BUTTON_R4;
    }
    return buttons;
}
