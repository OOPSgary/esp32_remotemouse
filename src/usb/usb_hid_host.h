#pragma once

#include "soc/soc_caps.h"

// Maximum number of simultaneously connected USB HID interfaces.
// A USB hub with keyboard + mouse uses 2 slots; composite devices with
// extra HID interfaces (media keys, etc.) may use more.
#define USB_HID_MAX_DEVICES 4

#if SOC_USB_OTG_SUPPORTED

// Initialise USB Host Library (hub driver included) and create internal
// state.  Devices connected through a USB hub / docking station are
// enumerated automatically by the built-in hub driver.
void usb_hid_host_init(void);

// Start background FreeRTOS tasks:
//   - USB Host Library daemon  (low-level USB event processing)
//   - USB HID client           (device enumeration, HID report polling)
void usb_hid_host_start_task(void);

#else
// Stubs for chips without USB OTG (e.g. original ESP32)
static inline void usb_hid_host_init(void) {}
static inline void usb_hid_host_start_task(void) {}
#endif
