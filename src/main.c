#include <btstack_run_loop.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <pico/async_context.h>
#include "usb.h"
#include "report.h"
#include "config.h"

// joypad-os Bluetooth stack — core1 entry (src/bt_hid/ns2_bt_host.c).
void ns2_bt_core_task(void);

int
main()
{
	stdio_init_all();

	// Initialize the cross-core shared input state before either core uses it.
	report_init();

	// Load persistent settings (lightbar colours, etc.) from flash.
	config_load();

	multicore_launch_core1(ns2_bt_core_task);
	usb_core_task();

	return 0;
}
