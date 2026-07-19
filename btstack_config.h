// btstack_config.h — minimal BTstack config for OpenDroneID BLE scan (LE central).
// Only used on CYW43 boards (Pico W / Pico Plus 2 W).
#ifndef HET68_BTSTACK_CONFIG_H
#define HET68_BTSTACK_CONFIG_H

#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LOG_ERROR
// Required by pico-sdk's hci_dump_embedded_stdout.c even if we never dump HCI.
#define ENABLE_PRINTF_HEXDUMP

#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_GATT_CLIENTS 0
#define MAX_NR_L2CAP_CHANNELS 1
#define MAX_NR_L2CAP_SERVICES 0
#define MAX_NR_SM_LOOKUP_ENTRIES 1
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 1

// Limit buffers — shared CYW43 bus with (unused) Wi-Fi path.
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 0
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 255
#define HCI_HOST_ACL_PACKET_NUM 2
#define HCI_HOST_SCO_PACKET_LEN 0
#define HCI_HOST_SCO_PACKET_NUM 0

#define NVM_NUM_DEVICE_DB_ENTRIES 4
#define NVM_NUM_LINK_KEYS 0
#define MAX_ATT_DB_SIZE 0

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT

#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#endif
