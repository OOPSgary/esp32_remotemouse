#include "hid_parser.h"
#include <string.h>

// Boot-protocol keyboard report: modifier(1) + reserved(1) + keycodes(6) = 8 bytes
// Boot-protocol mouse report:    buttons(1) + dx(1) + dy(1) [+ wheel(1)]   = 3-4 bytes
// We also tolerate longer reports (report ID prefix in some Report Protocol devices).

bool hid_parse_report(const uint8_t *data, uint16_t len,
                      HidDeviceRole role, HidPacket *out_pkt)
{
    if (!data || !out_pkt || len == 0) {
        return false;
    }

    out_pkt->magic[0] = 0xAB;
    out_pkt->magic[1] = 0xCD;

    if (role == HID_ROLE_KEYBOARD) {
        // Some devices prepend a 1-byte report-ID; skip it when present
        const uint8_t *p = data;
        uint16_t remaining = len;
        if (remaining == 9) {
            // report-ID prefix present
            p++;
            remaining--;
        }
        if (remaining < 8) {
            return false;
        }
        out_pkt->type = HID_EVENT_KEY;
        out_pkt->key.modifiers = p[0];
        // p[1] is the reserved byte – skip
        memcpy(out_pkt->key.keycodes, p + 2, 6);
        return true;

    } else if (role == HID_ROLE_MOUSE) {
        const uint8_t *p = data;
        uint16_t remaining = len;
        // Skip report-ID byte when the report is 4 or 5 bytes (boot = 3-4)
        if (remaining >= 4 && remaining <= 5) {
            // might have report-ID; check if first byte looks like a report-ID (non-zero)
            // For safety just use: if remaining==5, skip first byte
            if (remaining == 5) {
                p++;
                remaining--;
            }
        } else if (remaining == 4) {
            // standard boot mouse with wheel – no prefix
        } else if (remaining == 3) {
            // standard boot mouse without wheel – no prefix
        } else if (remaining > 5) {
            // Report Protocol with report-ID prefix; skip first byte
            p++;
            remaining--;
        }
        if (remaining < 3) {
            return false;
        }
        out_pkt->type = HID_EVENT_MOUSE;
        out_pkt->mouse.buttons = p[0];
        // dx/dy are signed 8-bit in Boot Protocol; sign-extend to int16
        out_pkt->mouse.dx    = (int16_t)(int8_t)p[1];
        out_pkt->mouse.dy    = (int16_t)(int8_t)p[2];
        out_pkt->mouse.wheel = (remaining >= 4) ? (int8_t)p[3] : 0;
        return true;
    }

    return false;
}
