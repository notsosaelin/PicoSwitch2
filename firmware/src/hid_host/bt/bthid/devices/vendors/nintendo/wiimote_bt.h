// wiimote_bt.h - Nintendo Wiimote Bluetooth Driver

#ifndef WIIMOTE_BT_H
#define WIIMOTE_BT_H

#include "bt/bthid/bthid.h"
#include <stdint.h>

extern const bthid_driver_t wiimote_bt_driver;

void wiimote_bt_register(void);

// Orientation mode: 0=Auto, 1=Horizontal, 2=Vertical
#define WII_ORIENT_MODE_AUTO       0
#define WII_ORIENT_MODE_HORIZONTAL 1
#define WII_ORIENT_MODE_VERTICAL   2

uint8_t wiimote_get_orient_mode(void);
void wiimote_set_orient_mode(uint8_t mode);
const char* wiimote_get_orient_mode_name(uint8_t mode);

#endif // WIIMOTE_BT_H
