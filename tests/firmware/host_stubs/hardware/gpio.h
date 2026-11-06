#ifndef _HOST_STUB_HARDWARE_GPIO_H_
#define _HOST_STUB_HARDWARE_GPIO_H_

// Host stand-in for hardware/gpio.h. Only the output-enable override values are needed; they
// keep the SDK's numbering so a stub cannot quietly invert the meaning of a CS write.

enum gpio_override {
    GPIO_OVERRIDE_NORMAL = 0,
    GPIO_OVERRIDE_INVERT = 1,
    GPIO_OVERRIDE_LOW = 2,
    GPIO_OVERRIDE_HIGH = 3,
};

#endif  // _HOST_STUB_HARDWARE_GPIO_H_
