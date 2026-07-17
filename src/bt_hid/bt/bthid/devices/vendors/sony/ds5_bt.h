// ds5_bt.h - Sony DualSense Bluetooth Driver
// Handles PS5 DualSense controllers over Bluetooth

#ifndef DS5_BT_H
#define DS5_BT_H

#include "bt/bthid/bthid.h"

// DualSense Bluetooth driver
extern const bthid_driver_t ds5_bt_driver;

// Register the DualSense BT driver
void ds5_bt_register(void);

// RP2350-only audio codec/transport service. Kept out of ordinary bthid_task()
// so Opus never runs from inside a deep inbound HID callback stack.
void ds5_bt_audio_service(void);

// Transport-only safe point for sustained inbound-report traffic. The
// diagnostic tone may also advance its trivial clock here, but live Opus
// encoding remains restricted to ds5_bt_audio_service()'s shallow timer.
void ds5_bt_audio_report_service(void);

#endif // DS5_BT_H
