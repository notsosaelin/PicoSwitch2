# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Target board:
#   - Pico W   (RP2040):  -DPICO_BOARD=pico_w   (default)
#   - Pico 2 W (RP2350):  -DPICO_BOARD=pico2_w
# Build each board in its own build directory; the artifact is named after the
# board (OUTPUT_NAME below) so both .uf2 files can coexist.
set(PICO_BOARD pico_w CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

# Use the BTstack bundled with the Pico SDK (current 2.x; no external btstack wiring).
set(BTSTACK_ROOT ${PICO_SDK_PATH}/lib/btstack)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(PicoSwitchWGA C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Re-run configuration when a source file is added or removed. Without
# CONFIGURE_DEPENDS, an existing build directory silently omits new modules.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.c")
list(APPEND SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/8Bitdo/integration/switch_pro_8bitdo.c")
add_executable(PicoSwitchWGA ${SOURCES})

# Name the binary after the board so pico_w / pico2_w outputs don't clobber.
set_target_properties(PicoSwitchWGA PROPERTIES OUTPUT_NAME "PicoSwitchWGA-${PICO_BOARD}")

# Number of emulated Pro Controllers (= USB HID interfaces). Single knob shared
# by the descriptors (usb_descriptors.c), the protocol (switch_pro.*) and TinyUSB
# (CFG_TUD_HID in tusb_config.h). Max 4 (the Switch supports 4 players).
target_compile_definitions(PicoSwitchWGA PRIVATE SWITCH_PRO_MAX_CONTROLLERS=4)

# Build target: emulate the Switch 2 Pro Controller (VID 057E / PID 2069) instead
# of the Switch 1 Pro Controller. Adds a vendor-bulk command interface (IF1) and
# the switch_pro2 protocol module (src/switch_pro2). See docs/switch2/usb-spec.md.
# Default ON on the ns2-testing branch; configure -DNS2_PRO=OFF for Switch 1.
option(NS2_PRO "Emulate the Switch 2 Pro Controller (PID 0x2069)" ON)
if(NS2_PRO)
    target_compile_definitions(PicoSwitchWGA PRIVATE NS2_PRO=1)
endif()

# NS2 Option A: present the full retail descriptor including the 3 USB audio
# interfaces. A PC2-specific UAC1 driver owns the two isochronous streams and
# Feature Unit controls. Only meaningful with NS2_PRO. -DNS2_AUDIO=OFF falls
# back to the 2-interface Option B.
option(NS2_AUDIO "NS2: include the audio interfaces (Option A, full descriptor)" ON)
if(NS2_PRO AND NS2_AUDIO)
    target_compile_definitions(PicoSwitchWGA PRIVATE NS2_AUDIO=1)
endif()

# Audible DualSense Bluetooth audio requires the RP2350 memory/CPU budget.
# The hardware-confirmed configuration is the opt-in 300 MHz live-audio build;
# ordinary Pico W/Pico 2 W firmware remains at its normal clock without Opus.
# The USB endpoint delivers exact 48 kHz stereo. DualSense Bluetooth consumes
# nominal 480-sample Opus frames at an effective 45 kHz clock, so the live
# bridge accumulates 512 real input frames and resamples them to 480 before
# encoding, matching both DS5Dongle and daidr/dualsense-tester.
option(NS2_DS5_AUDIO "NS2: experimental live USB-to-DualSense Opus bridge" OFF)
option(NS2_DS5_AUDIO_OVERCLOCK
       "NS2: overclock the RP2350 live-audio build" OFF)
set(NS2_DS5_AUDIO_OVERCLOCK_MHZ "200" CACHE STRING
    "Experimental RP2350 live-audio system clock in MHz")
option(NS2_DS5_AUDIO_TEST_TONE
       "NS2: diagnostic fixed DualSense tone (no live Opus encoder)" OFF)
if(NS2_DS5_AUDIO_OVERCLOCK AND NOT NS2_DS5_AUDIO)
    message(FATAL_ERROR "NS2_DS5_AUDIO_OVERCLOCK requires NS2_DS5_AUDIO=ON")
endif()
if(NS2_PRO AND NS2_AUDIO AND
   (NS2_DS5_AUDIO OR NS2_DS5_AUDIO_TEST_TONE) AND
   PICO_PLATFORM MATCHES "^rp2350")
    target_compile_definitions(PicoSwitchWGA PRIVATE NS2_DS5_AUDIO=1)
endif()

if(NS2_PRO AND NS2_AUDIO AND NS2_DS5_AUDIO AND
   PICO_PLATFORM MATCHES "^rp2350")
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
    target_compile_definitions(PicoSwitchWGA PRIVATE NS2_DS5_AUDIO_LIVE_OPUS=1)
    if(NS2_DS5_AUDIO_OVERCLOCK)
        if(NS2_DS5_AUDIO_OVERCLOCK_MHZ LESS_EQUAL 150 OR
           NS2_DS5_AUDIO_OVERCLOCK_MHZ GREATER 350)
            message(FATAL_ERROR
                    "Audio overclock must be greater than 150 and at most 350 MHz")
        endif()
        math(EXPR NS2_DS5_AUDIO_OVERCLOCK_KHZ
             "${NS2_DS5_AUDIO_OVERCLOCK_MHZ} * 1000")
        # Keep the CYW43 PIO bus at or below its normal 75 MHz rate:
        # 200/3 = 66.7 MHz and 300/4 = 75 MHz.
        math(EXPR NS2_DS5_CYW43_DIV
             "(${NS2_DS5_AUDIO_OVERCLOCK_MHZ} + 74) / 75")
        target_compile_definitions(PicoSwitchWGA PRIVATE
            NS2_DS5_AUDIO_OVERCLOCK_KHZ=${NS2_DS5_AUDIO_OVERCLOCK_KHZ}
            CYW43_PIO_CLOCK_DIV_INT=${NS2_DS5_CYW43_DIV})
    endif()
    # DS5Dongle's current RP2350 implementation relocates libopus code and
    # read-only tables into SRAM. Running the encoder from XIP flash caused
    # enough cache stalls to miss its real-time deadline even when the codec
    # had a dedicated core. It is more important here because codec and
    # Bluetooth share core1. The archive is about 220 KiB in RAM; the live
    # Pico 2 W image has ample headroom after its codec stack and queues.
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp
        COMMAND ${CMAKE_OBJCOPY}
                --rename-section .text=.time_critical.opus_text
                --rename-section .rodata=.time_critical.opus_rodata
                --rename-section .rodata.str1.4=.time_critical.opus_strings
                $<TARGET_FILE:opus>
        COMMAND ${CMAKE_COMMAND} -E touch
                ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp
        DEPENDS $<TARGET_FILE:opus>
        COMMENT "Relocating libopus hot code and tables to SRAM"
        VERBATIM
    )
    add_custom_target(opus_ram_relocate ALL
                      DEPENDS ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp)
    add_dependencies(PicoSwitchWGA opus_ram_relocate)
    set_source_files_properties(src/ds5_audio_ram_mem.c PROPERTIES
        COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")
    # The SDK's automatic core1 stack is linked into the 4 KiB SCRATCH_X bank,
    # which is too small for Opus. Disable that allocation; main.c supplies a
    # 48 KiB stack from ordinary SRAM via multicore_launch_core1_with_stack().
    # The pinned reference measured roughly 26 KiB at the encoder's deepest
    # point, leaving useful headroom for the foreground worker while the
    # core1 CYW43/BTstack background IRQ may preempt it.
    target_compile_definitions(PicoSwitchWGA PRIVATE PICO_CORE1_STACK_SIZE=0)
    target_link_libraries(PicoSwitchWGA opus)
endif()

if(NS2_PRO AND NS2_AUDIO AND NS2_DS5_AUDIO_TEST_TONE AND
   PICO_PLATFORM MATCHES "^rp2350")
    target_compile_definitions(PicoSwitchWGA PRIVATE
                               NS2_DS5_AUDIO_TEST_TONE=1)
endif()

# NS2 diagnostic: blink the onboard LED to report how far the console's init
# handshake progressed (see switch_pro2.c g_ns2_stage). Off by default.
option(NS2_DIAG "NS2: LED handshake-progress diagnostic" OFF)
if(NS2_PRO AND NS2_DIAG)
    target_compile_definitions(PicoSwitchWGA PRIVATE NS2_DIAG=1)
endif()

# joypad-os bthid Bluetooth stack.
target_include_directories(PicoSwitchWGA PRIVATE src/bt_hid src/bt_hid/bt/bthid)
# BTSTACK_USE_CYW43 selects the async_context run loop path in the vendored
# btstack_host/transport (matches the Pico W radio; compiles out the embedded
# run-loop calls). NS2_BT_ALL_DRIVERS compiles + registers every vendor driver.
target_compile_definitions(PicoSwitchWGA PRIVATE BTSTACK_USE_CYW43=1 NS2_BT_ALL_DRIVERS=1)

target_include_directories(PicoSwitchWGA PRIVATE
    src
    include
    8Bitdo/integration)

# `include` holds tusb_config.h; `src` holds btstack_config.h. These must be on the
# global include path so the SDK's TinyUSB and BTstack can find them.
include_directories(include src)

# Needed when using BTstack from the SDK (bluedroid codecs + btstack headers).
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)
include_directories(${BTSTACK_ROOT}/src)

target_link_libraries(PicoSwitchWGA
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_classic
    pico_btstack_ble
    pico_btstack_cyw43
    tinyusb_device
    tinyusb_board
    pico_multicore
    hardware_flash
)

# USB is used as the HID device to the console, so it cannot be a stdio CDC.
pico_enable_stdio_usb(PicoSwitchWGA 0)
pico_enable_stdio_uart(PicoSwitchWGA 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(PicoSwitchWGA)
