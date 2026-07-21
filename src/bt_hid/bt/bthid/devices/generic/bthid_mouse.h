#ifndef BTHID_MOUSE_H
#define BTHID_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

#include "bt/bthid/bthid.h"

extern const bthid_driver_t bthid_mouse_driver;

void bthid_mouse_register(void);
bool bthid_mouse_descriptor_is_mouse(const uint8_t *desc, uint16_t desc_len);
void bthid_mouse_set_descriptor(bthid_device_t *device,
                                const uint8_t *desc, uint16_t desc_len);

#endif
