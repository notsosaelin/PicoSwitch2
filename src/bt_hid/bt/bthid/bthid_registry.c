// bthid_registry.c - BTHID Driver Registration
// Registers all Bluetooth HID device drivers

#include "bthid_registry.h"
#include "bthid.h"

// Include all BT HID drivers
#include "devices/generic/bthid_gamepad.h"
#include "devices/generic/bthid_mouse.h"
#include "devices/vendors/sony/ds3_bt.h"
#include "devices/vendors/sony/ds4_bt.h"
#include "devices/vendors/sony/ds5_bt.h"
#include "devices/vendors/nintendo/switch_pro_bt.h"
#include "devices/vendors/nintendo/switch2_ble.h"
#include "devices/vendors/nintendo/wii_u_pro_bt.h"
#include "devices/vendors/nintendo/wiimote_bt.h"
#include "devices/vendors/microsoft/xbox_ble.h"
#include "devices/vendors/microsoft/xbox_bt.h"
#include "devices/vendors/retrofighters/battlergc_pro.h"
#include "devices/vendors/google/stadia_bt.h"
#include "devices/vendors/augmental/mouthpad_ble.h"

void bthid_registry_init(void)
{
    // Initialize BTHID layer
    bthid_init();

    // Register vendor-specific drivers first (higher priority)
    // Order matters - first match wins
    //
    // PicoSwitch2 port: the vendor drivers are gated behind NS2_BT_ALL_DRIVERS.
    // Phase 0 brings up ONLY the generic HID-descriptor driver (which already
    // covers all Xbox variants incl. Elite), so those .c files aren't compiled
    // yet (see CMakeLists.txt). Define NS2_BT_ALL_DRIVERS + compile the vendor
    // dirs to light up DualSense/DualShock, Switch, Stadia, etc.
#if defined(NS2_BT_ALL_DRIVERS) || defined(NS2_BT_SONY)
    // Sony controllers (DualShock 3/4, DualSense / DualSense Edge)
    ds3_bt_register();
    ds4_bt_register();
    ds5_bt_register();
#endif
#ifdef NS2_BT_ALL_DRIVERS
    // Nintendo controllers
    switch_pro_bt_register();
    switch2_ble_register();  // Switch 2 BLE controllers (Pro2, Joy-Con 2, GC NSO)
    wii_u_pro_bt_register();  // Must be before wiimote (Wii U Pro has "-UC" suffix)
    wiimote_bt_register();

    // BattlerGC deliberately impersonates the exact unresolved Classic Xbox
    // name, so its narrower model profile must run before the Xbox fallback.
    battlergc_pro_register();

    // Microsoft controllers — re-registered 2026-07-12 (previously deliberately left
    // unregistered in favor of the generic HID-descriptor driver "to cover all Xbox
    // variants without layout assumptions" — see git history). Re-enabled after an
    // audit found these files' input parsing to be evidence-backed:
    // xbox_ble.c's BLE button/stick parsing is a single, fixed 16-byte format
    // explicitly commented "verified from testing." xbox_bt.c (Classic BT) both
    // still exclude Xbox Elite Series 2 (product_id 0x0B05/0x0B22), which falls
    // through to the generic driver as before — these only claim standard/Series
    // controllers. xbox_bt.c's own input parsing guesses between two different
    // report struct layouts based on report length and has no equivalent "verified"
    // comment — lower confidence than xbox_ble.c; watch for input regressions on
    // Classic BT Xbox specifically and fall back to the generic driver (delete these
    // register calls, or add an xbox_bt_match() exclusion) if real hardware shows
    // its heuristic format detection picks the wrong struct.
    xbox_ble_register();
    xbox_bt_register();

    // Google controllers
    stadia_bt_register();

    // Augmental MouthPad (BLE mouse/keyboard/consumer — matches by name)
    mouthpad_ble_register();
#endif  // NS2_BT_ALL_DRIVERS

    // Generic mouse must precede the catch-all BLE gamepad fallback. Classic
    // mice match by Class-of-Device; descriptor-time reclassification in
    // bthid.c catches unnamed BLE HOGP mice.
    bthid_mouse_register();

    // Generic gamepad driver (fallback, lowest priority)
    bthid_gamepad_register();
}
