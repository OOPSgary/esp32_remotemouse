//go:build windows

// Package inject translates HidPacket events into Windows OS input via SendInput.
package inject

import (
	"fmt"
	"sync"
	"unsafe"

	"remotemouse/receiver"
	"golang.org/x/sys/windows"
)

var (
	user32         = windows.NewLazySystemDLL("user32.dll")
	procSendInput  = user32.NewProc("SendInput")
)

// INPUT types
const (
	inputMouse    = 0
	inputKeyboard = 1
)

// MOUSEEVENTF flags
const (
	mouseMOVE        = 0x0001
	mouseLEFTDOWN    = 0x0002
	mouseLEFTUP      = 0x0004
	mouseRIGHTDOWN   = 0x0008
	mouseRIGHTUP     = 0x0010
	mouseMIDDLEDOWN  = 0x0020
	mouseMIDDLEUP    = 0x0040
	mouseWHEEL       = 0x0800
)

// KEYEVENTF flags
const (
	keyEXTENDEDKEY = 0x0001
	keyKEYUP       = 0x0002
	keySCANCODE    = 0x0008
)

// mouseInput is the Windows MOUSEINPUT structure.
type mouseInput struct {
	dx          int32
	dy          int32
	mouseData   uint32
	dwFlags     uint32
	time        uint32
	dwExtraInfo uintptr
}

// keybdInput is the Windows KEYBDINPUT structure.
type keybdInput struct {
	wVk         uint16
	wScan       uint16
	dwFlags     uint32
	time        uint32
	dwExtraInfo uintptr
}

// input is the Windows INPUT structure (union: mouse or keyboard).
// On 64-bit Windows: DWORD type(4) + 4-byte padding + 28-byte union = 36 bytes.
// MOUSEINPUT is the largest member: dx(4)+dy(4)+mouseData(4)+dwFlags(4)+time(4)+dwExtraInfo(8)=28.
// KEYBDINPUT: wVk(2)+wScan(2)+dwFlags(4)+time(4)+dwExtraInfo(8)=20.
type input struct {
	inputType uint32
	_         [4]byte  // padding: Windows aligns the union to 8 bytes on 64-bit
	union     [28]byte // large enough for MOUSEINPUT (the biggest member)
}

func makeMouseInput(mi mouseInput) input {
	var in input
	in.inputType = inputMouse
	// mouseInput is 28 bytes on 64-bit (uintptr = 8)
	copy(in.union[:], (*[28]byte)(unsafe.Pointer(&mi))[:])
	return in
}

func makeKeybdInput(ki keybdInput) input {
	var in input
	in.inputType = inputKeyboard
	// keybdInput is 20 bytes on 64-bit
	copy(in.union[:20], (*[20]byte)(unsafe.Pointer(&ki))[:])
	return in
}

func sendInput(inputs []input) error {
	if len(inputs) == 0 {
		return nil
	}
	n, _, err := procSendInput.Call(
		uintptr(len(inputs)),
		uintptr(unsafe.Pointer(&inputs[0])),
		uintptr(unsafe.Sizeof(inputs[0])),
	)
	if n == 0 {
		return fmt.Errorf("SendInput failed: %w", err)
	}
	return nil
}

// Injector holds the state needed to diff keyboard frames and inject input.
type Injector struct {
	mu          sync.Mutex
	prevKeys    receiver.KeyEvent  // previous keyboard frame for key-up detection
	prevButtons uint8              // previous mouse button state
}

// New creates a new Injector. Close is a no-op but provided for symmetry.
func New() (*Injector, error) {
	return &Injector{}, nil
}

// Close releases resources (none currently).
func (inj *Injector) Close() {}

// Handle processes a HidPacket and injects the corresponding OS input events.
func (inj *Injector) Handle(pkt receiver.HidPacket) error {
	inj.mu.Lock()
	defer inj.mu.Unlock()

	switch pkt.Type {
	case receiver.EventTypeKey:
		return inj.injectKeyboard(pkt.Key)
	case receiver.EventTypeMouse:
		return inj.injectMouse(pkt.Mouse)
	}
	return nil
}

// injectKeyboard diffs current vs previous keyboard frame and fires key-down/up events.
func (inj *Injector) injectKeyboard(ev receiver.KeyEvent) error {
	var inputs []input

	// Modifier keys: each modifier bit maps to a VK code
	modVKs := [8]uint16{
		vkLControl, vkLShift, vkLAlt, vkLWin,
		vkRControl, vkRShift, vkRAlt, vkRWin,
	}
	prevMod := inj.prevKeys.Modifiers
	curMod  := ev.Modifiers
	for i, vk := range modVKs {
		bit := uint8(1 << i)
		wasDown := (prevMod & bit) != 0
		isDown  := (curMod  & bit) != 0
		if !wasDown && isDown {
			inputs = append(inputs, makeKeybdInput(keybdInput{wVk: vk}))
		} else if wasDown && !isDown {
			inputs = append(inputs, makeKeybdInput(keybdInput{wVk: vk, dwFlags: keyKEYUP}))
		}
	}

	// Regular keys: key-down for newly pressed, key-up for released
	prev := inj.prevKeys.Keycodes
	cur  := ev.Keycodes

	// Keys that appear in cur but not prev → key-down
	for _, kc := range cur {
		if kc == 0 {
			continue
		}
		if !containsKey(prev[:], kc) {
			vk := hidToVK(kc)
			if vk != 0 {
				inputs = append(inputs, makeKeybdInput(keybdInput{wVk: vk}))
			}
		}
	}
	// Keys that appear in prev but not cur → key-up
	for _, kc := range prev {
		if kc == 0 {
			continue
		}
		if !containsKey(cur[:], kc) {
			vk := hidToVK(kc)
			if vk != 0 {
				inputs = append(inputs, makeKeybdInput(keybdInput{wVk: vk, dwFlags: keyKEYUP}))
			}
		}
	}

	inj.prevKeys = ev

	if len(inputs) == 0 {
		return nil
	}
	return sendInput(inputs)
}

// injectMouse injects mouse movement, button, and wheel events.
func (inj *Injector) injectMouse(ev receiver.MouseEvent) error {
	var inputs []input

	// Movement
	if ev.Dx != 0 || ev.Dy != 0 {
		inputs = append(inputs, makeMouseInput(mouseInput{
			dx:      int32(ev.Dx),
			dy:      int32(ev.Dy),
			dwFlags: mouseMOVE,
		}))
	}

	// Buttons: diff against previous state
	type btnDesc struct {
		bit   uint8
		down  uint32
		up    uint32
	}
	buttons := []btnDesc{
		{0x01, mouseLEFTDOWN, mouseLEFTUP},
		{0x02, mouseRIGHTDOWN, mouseRIGHTUP},
		{0x04, mouseMIDDLEDOWN, mouseMIDDLEUP},
	}
	for _, b := range buttons {
		wasDown := (inj.prevButtons & b.bit) != 0
		isDown  := (ev.Buttons    & b.bit) != 0
		if !wasDown && isDown {
			inputs = append(inputs, makeMouseInput(mouseInput{dwFlags: b.down}))
		} else if wasDown && !isDown {
			inputs = append(inputs, makeMouseInput(mouseInput{dwFlags: b.up}))
		}
	}
	inj.prevButtons = ev.Buttons

	// Scroll wheel
	if ev.Wheel != 0 {
		inputs = append(inputs, makeMouseInput(mouseInput{
			mouseData: uint32(int32(ev.Wheel) * 120), // WHEEL_DELTA = 120
			dwFlags:   mouseWHEEL,
		}))
	}

	if len(inputs) == 0 {
		return nil
	}
	return sendInput(inputs)
}

// containsKey returns true if key is present in the slice.
func containsKey(slice []uint8, key uint8) bool {
	for _, k := range slice {
		if k == key {
			return true
		}
	}
	return false
}

// Virtual key codes used for modifier keys
const (
	vkLShift   = 0xA0
	vkRShift   = 0xA1
	vkLControl = 0xA2
	vkRControl = 0xA3
	vkLAlt     = 0xA4
	vkRAlt     = 0xA5
	vkLWin     = 0x5B
	vkRWin     = 0x5C
)

// hidToVK maps a USB HID Usage Page 07 (Keyboard/Keypad) usage ID to a
// Windows Virtual Key code. Only the most common keys are listed; unknown
// keycodes return 0 (which the caller skips).
func hidToVK(hid uint8) uint16 {
	switch hid {
	// Letters A-Z
	case 0x04: return 'A'
	case 0x05: return 'B'
	case 0x06: return 'C'
	case 0x07: return 'D'
	case 0x08: return 'E'
	case 0x09: return 'F'
	case 0x0A: return 'G'
	case 0x0B: return 'H'
	case 0x0C: return 'I'
	case 0x0D: return 'J'
	case 0x0E: return 'K'
	case 0x0F: return 'L'
	case 0x10: return 'M'
	case 0x11: return 'N'
	case 0x12: return 'O'
	case 0x13: return 'P'
	case 0x14: return 'Q'
	case 0x15: return 'R'
	case 0x16: return 'S'
	case 0x17: return 'T'
	case 0x18: return 'U'
	case 0x19: return 'V'
	case 0x1A: return 'W'
	case 0x1B: return 'X'
	case 0x1C: return 'Y'
	case 0x1D: return 'Z'
	// Digits 1-9, 0
	case 0x1E: return '1'
	case 0x1F: return '2'
	case 0x20: return '3'
	case 0x21: return '4'
	case 0x22: return '5'
	case 0x23: return '6'
	case 0x24: return '7'
	case 0x25: return '8'
	case 0x26: return '9'
	case 0x27: return '0'
	// Function keys
	case 0x3A: return 0x70 // F1
	case 0x3B: return 0x71 // F2
	case 0x3C: return 0x72 // F3
	case 0x3D: return 0x73 // F4
	case 0x3E: return 0x74 // F5
	case 0x3F: return 0x75 // F6
	case 0x40: return 0x76 // F7
	case 0x41: return 0x77 // F8
	case 0x42: return 0x78 // F9
	case 0x43: return 0x79 // F10
	case 0x44: return 0x7A // F11
	case 0x45: return 0x7B // F12
	// Control keys
	case 0x28: return 0x0D // Enter / Return
	case 0x29: return 0x1B // Escape
	case 0x2A: return 0x08 // Backspace
	case 0x2B: return 0x09 // Tab
	case 0x2C: return 0x20 // Space
	case 0x2D: return 0xBD // Minus (-)
	case 0x2E: return 0xBB // Equals (=)
	case 0x2F: return 0xDB // Left bracket [
	case 0x30: return 0xDD // Right bracket ]
	case 0x31: return 0xDC // Backslash
	case 0x33: return 0xBA // Semicolon
	case 0x34: return 0xDE // Quote
	case 0x35: return 0xC0 // Grave accent / tilde
	case 0x36: return 0xBC // Comma
	case 0x37: return 0xBE // Period
	case 0x38: return 0xBF // Slash
	case 0x39: return 0x14 // Caps Lock
	case 0x46: return 0x2C // Print Screen
	case 0x47: return 0x91 // Scroll Lock
	case 0x48: return 0x13 // Pause/Break
	case 0x49: return 0x2D // Insert
	case 0x4A: return 0x24 // Home
	case 0x4B: return 0x21 // Page Up
	case 0x4C: return 0x2E // Delete
	case 0x4D: return 0x23 // End
	case 0x4E: return 0x22 // Page Down
	case 0x4F: return 0x27 // Right arrow
	case 0x50: return 0x25 // Left arrow
	case 0x51: return 0x28 // Down arrow
	case 0x52: return 0x26 // Up arrow
	case 0x53: return 0x90 // Num Lock
	// Numpad
	case 0x54: return 0x6F // Numpad /
	case 0x55: return 0x6A // Numpad *
	case 0x56: return 0x6D // Numpad -
	case 0x57: return 0x6B // Numpad +
	case 0x58: return 0x0D // Numpad Enter (same VK as Enter)
	case 0x59: return 0x61 // Numpad 1
	case 0x5A: return 0x62 // Numpad 2
	case 0x5B: return 0x63 // Numpad 3
	case 0x5C: return 0x64 // Numpad 4
	case 0x5D: return 0x65 // Numpad 5
	case 0x5E: return 0x66 // Numpad 6
	case 0x5F: return 0x67 // Numpad 7
	case 0x60: return 0x68 // Numpad 8
	case 0x61: return 0x69 // Numpad 9
	case 0x62: return 0x60 // Numpad 0
	case 0x63: return 0x6E // Numpad .
	// Media keys (via HID Consumer page usage translated by some keyboards)
	case 0x7F: return 0xAD // Mute
	case 0x80: return 0xAF // Volume Up
	case 0x81: return 0xAE // Volume Down
	}
	return 0 // unknown/unmapped
}
