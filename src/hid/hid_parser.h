#pragma once
#include "hid_types.h"
#include <stdint.h>

// Device role hint used by the parser to select the correct report layout
enum HidDeviceRole {
    HID_ROLE_KEYBOARD,
    HID_ROLE_MOUSE,
};

// Parse a raw HID boot-protocol report into a KeyEvent or MouseEvent.
// Returns true on success and fills out_pkt; returns false if the report
// does not match the expected length or device role.
bool hid_parse_report(const uint8_t *data, uint16_t len,
                      HidDeviceRole role, HidPacket *out_pkt);
