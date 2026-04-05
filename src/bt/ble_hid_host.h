#pragma once
#include <stdint.h>

// Maximum number of simultaneously paired/connected BLE HID devices
#define BLE_HID_MAX_DEVICES 2

// Initialise BLE stack, register GAP/GATTC callbacks, and start advertising
// scan for HID peripherals (UUID 0x1812).
// Discovered reports are pushed to the global hid_event_queue created in main.cpp.
void ble_hid_host_init(void);

// Start a background FreeRTOS task that continuously scans for BLE HID devices
// and reconnects to devices stored in NVS pairing store.
void ble_hid_host_start_task(void);
