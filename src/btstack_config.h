#ifndef _PICO_BTSTACK_BTSTACK_CONFIG_H
#define _PICO_BTSTACK_BTSTACK_CONFIG_H

// Copy & paste from, with some custom changes:
// https://github.com/raspberrypi/pico-examples/blob/master/pico_w/bt/config/btstack_config.h

// BTstack features that can be enabled
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_SCO_OVER_HCI

#ifdef ENABLE_BLE
#define ENABLE_GATT_CLIENT_PAIRING
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION
#define ENABLE_LE_SECURE_CONNECTIONS
#else
#error "BP32: ENABLE_BLE should be defined"
#endif

#ifdef ENABLE_CLASSIC
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#define ENABLE_GOEP_L2CAP
#else
#error "BP32: ENABLE_CLASSIC should be defined"
#endif

#if defined(ENABLE_CLASSIC) && defined(ENABLE_BLE)
#define ENABLE_CROSS_TRANSPORT_KEY_DERIVATION
#endif

// BTstack configuration. buffers, sizes, ...
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
#define MAX_NR_AVRCP_CONNECTIONS 2
#define MAX_NR_BNEP_CHANNELS 1
#define MAX_NR_BNEP_SERVICES 1
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 2
#define MAX_NR_GATT_CLIENTS 4
#define MAX_NR_HCI_CONNECTIONS 4
#define MAX_NR_HID_HOST_CONNECTIONS 4
#define MAX_NR_HIDS_CLIENTS 4
#define MAX_NR_HFP_CONNECTIONS 1
#define MAX_NR_L2CAP_CHANNELS 6
#define MAX_NR_L2CAP_SERVICES 5
#define MAX_NR_RFCOMM_CHANNELS 1
#define MAX_NR_RFCOMM_MULTIPLEXERS 1
#define MAX_NR_RFCOMM_SERVICES 1
#define MAX_NR_SERVICE_RECORD_ITEMS 4
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 16
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16

// ACL/SCO flow control. The conservative counts below are the pico-examples
// defaults, sized to protect the CYW43 SPI bus when WiFi and Bluetooth share it.
// This firmware is BT-only (pico_cyw43_arch_none): there is no WiFi traffic on
// that bus, so the same protection needlessly throttles the outgoing pipeline.
// Continuous DualSense speaker audio is ~50 x 548-byte reports/s, and a 3-deep
// pipeline plus 3-buffer host flow control gates it well below that, producing
// the choppy/half-rate tone observed on hardware. Audio builds
// (NS2_DS5_AUDIO_BT_DEEP_PIPELINE, injected from CMake before pico_sdk_init so
// the BTstack library inherits it) deepen the pipeline toward the BT-only
// DS5Dongle reference. Ordinary builds keep the hardware-confirmed values.
//
// See docs/switch2/dualsense-audio-bridge.md and the 2026-07-17 experiment log.
#ifdef NS2_DS5_AUDIO_BT_DEEP_PIPELINE
#define MAX_NR_CONTROLLER_ACL_BUFFERS 12
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 12
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3
#else
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3
#endif

// Link Key DB and LE Device DB using TLV on top of Flash Sector interface
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

// We don't give btstack a malloc, so use a fixed-size ATT DB.
#define MAX_ATT_DB_SIZE 512

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS

// map btstack_assert onto Pico SDK assert()
#define HAVE_ASSERT

// Some USB dongles take longer to respond to HCI reset (e.g. BCM20702A).
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#define HAVE_BTSTACK_STDIN

// To get the audio demos working even with HCI dump at 115200, this truncates long ACL packets
// #define HCI_DUMP_STDOUT_MAX_SIZE_ACL 100

#endif  // _PICO_BTSTACK_BTSTACK_CONFIG_H
