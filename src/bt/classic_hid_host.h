#pragma once

// Maximum number of simultaneously connected Classic BT HID devices
#define CLASSIC_HID_MAX_DEVICES 2

// Initialise the Classic Bluetooth HID Host using esp_hid_host API.
// Reports are pushed to the global hid_event_queue.
void classic_hid_host_init(void);

// Start a background FreeRTOS task that runs Inquiry scans for Classic BT HID
// devices and attempts reconnection to NVS-stored paired devices.
void classic_hid_host_start_task(void);
