// bthid_registry.c - BTHID Driver Registration
// Registers all Bluetooth HID device drivers

#include "bthid_registry.h"
#include "bthid.h"

// Include all BT HID drivers
#include "devices/generic/bthid_gamepad.h"
#include "devices/vendors/sony/ds3_bt.h"
#include "devices/vendors/sony/ds4_bt.h"
#include "devices/vendors/sony/ds5_bt.h"
#include "devices/vendors/nintendo/switch_pro_bt.h"
#include "devices/vendors/nintendo/switch2_ble.h"
#include "devices/vendors/nintendo/wii_u_pro_bt.h"
#include "devices/vendors/nintendo/wiimote_bt.h"
// xbox_bt.h and xbox_ble.h no longer registered — generic driver handles all Xbox
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

    // Microsoft controllers — handled by generic gamepad driver via HID descriptor
    // parsing (like BlueRetro). Covers all Xbox variants without layout assumptions.

    // Google controllers
    stadia_bt_register();

    // Augmental MouthPad (BLE mouse/keyboard/consumer — matches by name)
    mouthpad_ble_register();
#endif  // NS2_BT_ALL_DRIVERS

    // Generic gamepad driver (fallback, lowest priority)
    bthid_gamepad_register();
}
