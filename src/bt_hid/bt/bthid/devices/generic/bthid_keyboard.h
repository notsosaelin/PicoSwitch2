#ifndef BTHID_KEYBOARD_H
#define BTHID_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "bt/bthid/bthid.h"

extern const bthid_driver_t bthid_keyboard_driver;

void bthid_keyboard_register(void);

// Structural test used by bthid.c to reclassify a device once its report
// descriptor arrives. BLE has no Class of Device and keyboards do not
// advertise a reliable name, so the descriptor is the authority.
bool bthid_keyboard_descriptor_is_keyboard(const uint8_t *desc, uint16_t desc_len);

// Hand the descriptor to a bound keyboard device. A descriptor that also
// declares a pointer collection makes this one peer a combo keyboard+mouse,
// which the KB/M role registry may admit for both roles.
void bthid_keyboard_set_descriptor(bthid_device_t *device, const uint8_t *desc,
                                   uint16_t desc_len);

#endif  // BTHID_KEYBOARD_H
