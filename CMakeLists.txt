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
# Live encoding remains opt-in while its updated scheduler is hardware-tested.
# The USB endpoint already delivers exact 48 kHz stereo; the live bridge
# accumulates ten 1 ms packets directly into each 480-frame Opus window.
option(NS2_DS5_AUDIO "NS2: experimental live USB-to-DualSense Opus bridge" OFF)
option(NS2_DS5_AUDIO_TEST_TONE
       "NS2: diagnostic fixed DualSense tone (no live Opus encoder)" OFF)
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
    # The SDK's automatic core1 stack is linked into the 4 KiB SCRATCH_X bank,
    # which is too small for Opus. Disable that allocation; main.c supplies a
    # 48 KiB stack from ordinary SRAM via multicore_launch_core1_with_stack().
    # The pinned reference measured roughly 26 KiB at the encoder's deepest
    # point, leaving useful headroom for the surrounding BTstack timer call.
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
