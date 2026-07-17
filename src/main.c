#include <btstack_run_loop.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <pico/async_context.h>
#include "usb.h"
#include "report.h"
#include "config.h"
#include "ds5_audio_bridge.h"

// joypad-os Bluetooth stack — core1 entry (src/bt_hid/ns2_bt_host.c).
void ns2_bt_core_task(void);

#ifdef NS2_DS5_AUDIO_LIVE_OPUS
// Opus cannot use the SDK's normal core1 stack allocation: that section lives
// in the RP2350's 4 KiB SCRATCH_X bank. Keep the larger codec stack in ordinary
// SRAM and launch core1 explicitly with it.
#define NS2_AUDIO_CORE1_STACK_BYTES (48u * 1024u)
static uint32_t core1_audio_stack[NS2_AUDIO_CORE1_STACK_BYTES / sizeof(uint32_t)]
	__attribute__((aligned(8)));
#endif

int
main()
{
	stdio_init_all();

	// Initialize the cross-core shared input state before either core uses it.
	report_init();

	// Load persistent settings (controller body/lightbar colour, mappings, etc.) from flash.
	config_load();
	ds5_audio_bridge_init();

#ifdef NS2_DS5_AUDIO_LIVE_OPUS
	multicore_launch_core1_with_stack(ns2_bt_core_task, core1_audio_stack,
	                                 sizeof(core1_audio_stack));
#else
	multicore_launch_core1(ns2_bt_core_task);
#endif
	usb_core_task();

	return 0;
}
