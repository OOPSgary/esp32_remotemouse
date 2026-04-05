#pragma once
#include <stdint.h>

// Unified event types produced by both BLE and Classic BT HID paths
enum HidEventType : uint8_t {
    HID_EVENT_KEY   = 0x01,
    HID_EVENT_MOUSE = 0x02,
};

// Keyboard boot-protocol report (8 bytes: modifiers + reserved + 6 keycodes)
struct KeyEvent {
    uint8_t modifiers;    // bit mask: Ctrl/Shift/Alt/GUI left+right
    uint8_t keycodes[6];  // up to 6 simultaneous HID usage codes
};

// Mouse relative movement event
struct MouseEvent {
    int16_t dx;       // relative X movement (little-endian)
    int16_t dy;       // relative Y movement (little-endian)
    int8_t  wheel;    // scroll wheel delta
    uint8_t buttons;  // bit0=left, bit1=right, bit2=middle
};

// UDP packet sent over Wi-Fi to the Windows host (packed, no padding)
#pragma pack(push, 1)
struct HidPacket {
    uint8_t magic[2]; // always 0xAB 0xCD – sanity check on receiver
    uint8_t type;     // HidEventType
    union {
        KeyEvent   key;
        MouseEvent mouse;
    };
};
#pragma pack(pop)
