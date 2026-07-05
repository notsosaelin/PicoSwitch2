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

set(BLUEPAD32_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/bluepad32)

# Use the BTstack bundled with the Pico SDK (matches the upstream bluepad32
# pico_w example). The SDK 2.x BTstack is current and avoids the extra
# "missing sources" wiring that the bluepad32/external/btstack path needs.
set(BTSTACK_ROOT ${PICO_SDK_PATH}/lib/btstack)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)

project(PicoSwitchWGA C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

file(GLOB_RECURSE SOURCES "src/*.c")
# The vendored joypad-os Bluetooth stack (src/bt_hid/, added by ndeadly research;
# see docs/bluetooth/) is compiled ONLY under -DBT_STACK_JOYPAD=ON, wired in below.
# Keep it out of the default glob so a work-in-progress port never breaks the
# shipping bluepad32 build.
list(FILTER SOURCES EXCLUDE REGEX "/bt_hid/")
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
# interfaces (enumerated by a stub class driver; no functional audio). Only
# meaningful with NS2_PRO. -DNS2_AUDIO=OFF falls back to the 2-interface Option B.
option(NS2_AUDIO "NS2: include the audio interfaces (Option A, full descriptor)" ON)
if(NS2_PRO AND NS2_AUDIO)
    target_compile_definitions(PicoSwitchWGA PRIVATE NS2_AUDIO=1)
endif()

# NS2 diagnostic: blink the onboard LED to report how far the console's init
# handshake progressed (see switch_pro2.c g_ns2_stage). Off by default.
option(NS2_DIAG "NS2: LED handshake-progress diagnostic" OFF)
if(NS2_PRO AND NS2_DIAG)
    target_compile_definitions(PicoSwitchWGA PRIVATE NS2_DIAG=1)
endif()

# Bluetooth stack selection. Default (OFF) = bluepad32, the shipping stack. ON =
# the native BTstack/joypad-os HID layer vendored under src/bt_hid/, which exposes
# every button (GL/GR/C, Xbox Elite paddles, DualSense Edge) and enables audio
# later. Work in progress — see docs/bluetooth/bt-stack-migration.md. Sources are
# added here only when enabled (they are excluded from the default glob above).
option(BT_STACK_JOYPAD "Use the joypad-os bthid Bluetooth stack instead of bluepad32 (WIP)" OFF)
if(BT_STACK_JOYPAD)
    file(GLOB_RECURSE BT_HID_SOURCES "src/bt_hid/*.c")
    # All joypad-os controller drivers: generic (Xbox incl. Elite) + Sony (DualShock
    # 3/4, DualSense/Edge) + Nintendo (Switch Pro, Switch 2 BLE, Wii U Pro, Wiimote) +
    # Google (Stadia) + Augmental (MouthPad). Registered via NS2_BT_ALL_DRIVERS.
    target_sources(PicoSwitchWGA PRIVATE ${BT_HID_SOURCES})
    target_include_directories(PicoSwitchWGA PRIVATE src/bt_hid src/bt_hid/bt/bthid)
    # BTSTACK_USE_CYW43 selects the async_context run loop path in the vendored
    # btstack_host/transport (matches the Pico W radio; compiles out the embedded
    # run-loop calls). NS2_BT_ALL_DRIVERS compiles + registers every vendor driver.
    target_compile_definitions(PicoSwitchWGA PRIVATE BT_STACK_JOYPAD=1 BTSTACK_USE_CYW43=1 NS2_BT_ALL_DRIVERS=1)
endif()

target_include_directories(PicoSwitchWGA PRIVATE
    src
    include
    ${BLUEPAD32_ROOT}/src/components/bluepad32/include)

# `include` holds tusb_config.h; `src` holds btstack_config.h / sdkconfig.h.
# These must be on the global include path so the SDK's TinyUSB and BTstack
# (and libbluepad32) can find them.
include_directories(include src)

# Needed when using BTstack from the SDK (bluedroid codecs + btstack headers).
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)
include_directories(${BTSTACK_ROOT}/src)

target_link_libraries(PicoSwitchWGA
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_classic
    pico_btstack_cyw43
    bluepad32
    tinyusb_device
    tinyusb_board
    pico_multicore
    hardware_flash
)

add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)

# USB is used as the HID device to the console, so it cannot be a stdio CDC.
pico_enable_stdio_usb(PicoSwitchWGA 0)
pico_enable_stdio_uart(PicoSwitchWGA 0)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(PicoSwitchWGA)
