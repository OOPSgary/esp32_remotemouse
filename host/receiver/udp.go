//go:build windows

// Package receiver listens on a UDP port for HidPacket datagrams sent by the
// ESP32 firmware and calls a user-supplied callback for each valid packet.
package receiver

import (
	"encoding/binary"
	"fmt"
	"net"
)

const (
	magicByte0 = 0xAB
	magicByte1 = 0xCD

	EventTypeKey   = 0x01
	EventTypeMouse = 0x02
)

// KeyEvent mirrors the C KeyEvent struct (little-endian packed).
type KeyEvent struct {
	Modifiers uint8
	Keycodes  [6]uint8
}

// MouseEvent mirrors the C MouseEvent struct (little-endian packed).
type MouseEvent struct {
	Dx      int16
	Dy      int16
	Wheel   int8
	Buttons uint8
}

// HidPacket is the parsed representation of one UDP datagram from the ESP32.
type HidPacket struct {
	Type  uint8
	Key   KeyEvent   // valid when Type == EventTypeKey
	Mouse MouseEvent // valid when Type == EventTypeMouse
}

// rawPacket is the on-wire layout (matches the C HidPacket with #pragma pack(1)).
// Total size: 2 (magic) + 1 (type) + max(KeyEvent=7, MouseEvent=6) = 10 bytes.
// We read a fixed 10 bytes; the union occupies the larger of the two payloads.
const packetSize = 10

// parsePacket decodes a raw byte slice into a HidPacket.
// Returns an error if the magic bytes are wrong or the type is unknown.
func parsePacket(buf []byte) (HidPacket, error) {
	if len(buf) < packetSize {
		return HidPacket{}, fmt.Errorf("packet too short: %d < %d", len(buf), packetSize)
	}
	if buf[0] != magicByte0 || buf[1] != magicByte1 {
		return HidPacket{}, fmt.Errorf("bad magic: %02x %02x", buf[0], buf[1])
	}

	pkt := HidPacket{Type: buf[2]}
	payload := buf[3:]

	switch pkt.Type {
	case EventTypeKey:
		// modifier(1) + keycodes(6) = 7 bytes
		if len(payload) < 7 {
			return HidPacket{}, fmt.Errorf("key payload too short")
		}
		pkt.Key.Modifiers = payload[0]
		copy(pkt.Key.Keycodes[:], payload[1:7])

	case EventTypeMouse:
		// dx(2) + dy(2) + wheel(1) + buttons(1) = 6 bytes, little-endian
		if len(payload) < 6 {
			return HidPacket{}, fmt.Errorf("mouse payload too short")
		}
		pkt.Mouse.Dx      = int16(binary.LittleEndian.Uint16(payload[0:2]))
		pkt.Mouse.Dy      = int16(binary.LittleEndian.Uint16(payload[2:4]))
		pkt.Mouse.Wheel   = int8(payload[4])
		pkt.Mouse.Buttons = payload[5]

	default:
		return HidPacket{}, fmt.Errorf("unknown event type: 0x%02x", pkt.Type)
	}
	return pkt, nil
}

// Listen binds a UDP socket on addr (e.g. ":10086") and calls handler for
// each valid HidPacket received. It blocks until an unrecoverable error occurs.
func Listen(addr string, handler func(HidPacket)) error {
	conn, err := net.ListenPacket("udp", addr)
	if err != nil {
		return fmt.Errorf("net.ListenPacket(%q): %w", addr, err)
	}
	defer conn.Close()

	buf := make([]byte, 64) // larger than packetSize to catch oversized packets
	for {
		n, _, err := conn.ReadFrom(buf)
		if err != nil {
			return fmt.Errorf("ReadFrom: %w", err)
		}
		pkt, err := parsePacket(buf[:n])
		if err != nil {
			// Silently drop malformed packets; don't crash the service
			continue
		}
		handler(pkt)
	}
}
