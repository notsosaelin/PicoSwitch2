#include <btstack_run_loop.h>
#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <pico/async_context.h>
#ifdef NS2_PICO2_SYSTEM_CLOCK_KHZ
#include <hardware/clocks.h>
#include <hardware/vreg.h>
#endif
#include "usb.h"
#include "report.h"
#include "config.h"
#include "ds5_audio_bridge.h"
#include "ns2_uart_diag.h"

// joypad-os Bluetooth stack — core1 entry (src/bt_hid/ns2_bt_host.c).
void ns2_bt_core_task(void);

#ifdef NS2_DS5_AUDIO_LIVE_OPUS
// Opus cannot use the SDK's normal core1 stack allocation. Keep the codec/BT
// IRQ stack in ordinary SRAM and launch core1 explicitly with it.
#ifndef NS2_AUDIO_CORE1_STACK_BYTES
#define NS2_AUDIO_CORE1_STACK_BYTES (48u * 1024u)
#endif
#define NS2_AUDIO_STACK_FILL 0xA5A5A5A5u
static uint32_t core1_audio_stack[NS2_AUDIO_CORE1_STACK_BYTES / sizeof(uint32_t)]
	__attribute__((aligned(8)));
#endif

uint32_t ns2_audio_core1_stack_free_bytes(void)
{
#ifdef NS2_DS5_AUDIO_LIVE_OPUS
	size_t free_words = 0;
	while (free_words < NS2_AUDIO_CORE1_STACK_BYTES / sizeof(uint32_t) &&
	       core1_audio_stack[free_words] == NS2_AUDIO_STACK_FILL)
		free_words++;
	return (uint32_t)(free_words * sizeof(uint32_t));
#else
	return 0;
#endif
}

int
main()
{
#ifdef NS2_PICO2_SYSTEM_CLOCK_KHZ
	// Apply the validated Pico 2 W clock before USB or CYW43 is initialized.
	// CMake selects a CYW43 PIO divider that keeps its bus at or below 75 MHz.
	vreg_set_voltage(VREG_VOLTAGE_1_20);
	sleep_ms(1000);
	set_sys_clock_khz(NS2_PICO2_SYSTEM_CLOCK_KHZ, true);
#endif

	stdio_init_all();
	ns2_uart_diag_init();

	// Initialize the cross-core shared input state before either core uses it.
	report_init();

	// Load persistent settings (controller body/lightbar colour, mappings, etc.) from flash.
	config_load();
	ds5_audio_bridge_init();

#ifdef NS2_DS5_AUDIO_LIVE_OPUS
	for (size_t i = 0; i < NS2_AUDIO_CORE1_STACK_BYTES / sizeof(uint32_t); ++i)
		core1_audio_stack[i] = NS2_AUDIO_STACK_FILL;
	multicore_launch_core1_with_stack(ns2_bt_core_task, core1_audio_stack,
	                                 sizeof(core1_audio_stack));
#else
	multicore_launch_core1(ns2_bt_core_task);
#endif
	usb_core_task();

	return 0;
}
