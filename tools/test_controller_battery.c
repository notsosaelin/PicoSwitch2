#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "controller_battery.h"
#include "core/input_event.h"

static int failures;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            printf("FAIL: %s\n", message);                                     \
            failures++;                                                       \
        }                                                                      \
    } while (0)

static void check_battery(bool decoded, const controller_battery_t *battery,
                          uint8_t level, bool charging, const char *message)
{
    if (!decoded || battery->level != level ||
        battery->charging != charging) {
        printf("FAIL: %s (decoded=%d level=%u charging=%d)\n", message,
               decoded, battery->level, battery->charging);
        failures++;
    }
}

int main(void)
{
    controller_battery_t battery = {0};
    input_event_t event;
    init_input_event(&event);

    CHECK(event.battery_source == INPUT_BATTERY_NONE,
          "new controller event starts with unknown battery");
    CHECK(input_event_set_bas_battery(&event, 0),
          "BLE BAS accepts a genuine empty reading");
    CHECK(event.battery_source == INPUT_BATTERY_BAS &&
              event.battery_level == 0,
          "BLE BAS zero remains valid instead of unknown");
    CHECK(input_event_set_bas_battery(&event, 63) &&
              event.battery_level == 63,
          "recurring BLE BAS notifications update the cached percentage");
    input_event_set_native_battery(&event, 45, true);
    CHECK(event.battery_source == INPUT_BATTERY_NATIVE_HID &&
              event.battery_level == 45 && event.battery_charging,
          "native HID battery overrides a prior BAS fallback");
    CHECK(!input_event_set_bas_battery(&event, 20) &&
              event.battery_level == 45 && event.battery_charging,
          "later BAS notifications cannot overwrite native HID telemetry");

    check_battery(controller_battery_decode_ds3(0, &battery), &battery, 0,
                  false, "DualShock 3 empty is a valid 0 percent");
    check_battery(controller_battery_decode_ds3(3, &battery), &battery, 50,
                  false, "DualShock 3 discharge lookup");
    check_battery(controller_battery_decode_ds3(0xEE, &battery), &battery, 100,
                  true, "DualShock 3 charging status");
    check_battery(controller_battery_decode_ds3(0xEF, &battery), &battery, 100,
                  false, "DualShock 3 full status");
    CHECK(!controller_battery_decode_ds3(0x80, &battery),
          "DualShock 3 unknown status is rejected");

    check_battery(controller_battery_decode_ds4(0x04, &battery), &battery, 45,
                  false, "DualShock 4 wireless level");
    check_battery(controller_battery_decode_ds4(0x14, &battery), &battery, 45,
                  true, "DualShock 4 charging level");
    check_battery(controller_battery_decode_ds4(0x1B, &battery), &battery, 100,
                  false, "DualShock 4 cable-connected full status");
    CHECK(!controller_battery_decode_ds4(0x1E, &battery),
          "DualShock 4 error status is rejected");

    check_battery(controller_battery_decode_ds5(0x04, &battery), &battery, 45,
                  false, "DualSense discharging level");
    check_battery(controller_battery_decode_ds5(0x14, &battery), &battery, 45,
                  true, "DualSense charging level");
    check_battery(controller_battery_decode_ds5(0x20, &battery), &battery, 100,
                  false, "DualSense full status");
    CHECK(!controller_battery_decode_ds5(0xA4, &battery),
          "DualSense error status is rejected");

    check_battery(controller_battery_decode_switch_pro(0x40, &battery),
                  &battery, 53, false, "Switch Pro native level");
    check_battery(controller_battery_decode_switch_pro(0x68, &battery),
                  &battery, 77, true, "Switch Pro charging bit");
    check_battery(controller_battery_decode_wii_u_pro(0x24, &battery),
                  &battery, 50, false, "Wii U Pro native level");
    check_battery(controller_battery_decode_wii_u_pro(0x20, &battery),
                  &battery, 50, true, "Wii U Pro active-low charging bit");
    check_battery(controller_battery_decode_wiimote(0x80, &battery),
                  &battery, 50, false, "Wiimote half-scale level");
    check_battery(controller_battery_decode_wiimote(0xC0, &battery),
                  &battery, 75, false, "Wiimote status uses the full byte range");
    check_battery(controller_battery_decode_wiimote(0xFF, &battery),
                  &battery, 100, false, "Wiimote over-range clamps");

    CHECK(controller_battery_switch1_connection_info(false, 0) == 0x81,
          "Switch 1 unknown battery preserves full wired fallback");
    CHECK(controller_battery_switch1_connection_info(true, 0) == 0x01,
          "Switch 1 valid empty battery is not confused with unknown");
    CHECK(controller_battery_switch1_connection_info(true, 50) == 0x41,
          "Switch 1 half battery quantizes to native level 4");
    CHECK(controller_battery_switch1_connection_info(true, 100) == 0x81,
          "Switch 1 full battery remains wired level 8");

    CHECK(controller_battery_switch2_power_info(false, 0, true) == 0x25,
          "Switch 2 unknown battery preserves safe fallback");
    CHECK(controller_battery_switch2_power_info(true, 0, false) == 0x01,
          "Switch 2 valid empty battery is not confused with unknown");
    CHECK(controller_battery_switch2_power_info(true, 50, false) == 0x15,
          "Switch 2 half battery quantizes to level 5");
    CHECK(controller_battery_switch2_power_info(true, 50, true) == 0x17,
          "Switch 2 charging flag combines with remote level");
    CHECK(controller_battery_switch2_power_info(true, 100, false) == 0x25,
          "Switch 2 full battery matches previous power byte");

    if (failures) {
        printf("controller_battery: %d failure(s)\n", failures);
        return 1;
    }
    puts("controller_battery: all controller/source/encoder tests passed");
    return 0;
}
