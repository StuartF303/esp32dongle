// usbdongle W3 — vendored en_GB keyboard layout for the `hid` module.
//
// The Arduino-ESP32 core ships KeyboardLayout_en_US and a handful of others
// (de_DE, fr_FR, ...), but NO en_GB. stuart's host is a `gb`/pc105 keyboard
// (verified with setxkbmap), and HID transmits SCANCODES, not characters — the
// HOST applies its own layout. With the default en_US asciimap, several ASCII
// characters come out wrong on a GB host (@ <-> ", # -> £, \ -> #, ~ -> ¬, ...).
//
// This table is the ASCII->scancode map in the EXACT same format as the core's
// KeyboardLayout_en_US.cpp (see keyboard_layout_en_gb.cpp for the encoding and
// the one place the format cannot represent a UK key). It is a drop-in for
// USBHIDKeyboard::begin(const uint8_t *layout).
//
// LAYOUT IS A PROPERTY OF THE HOST, NOT THE DONGLE. The same macro typed into a
// US-layout machine produces different characters. That is why it is a per-host
// module setting, not a build constant.

#pragma once

#include <stdint.h>

// 128 entries, ASCII order, same shape as KeyboardLayout_en_US.
extern const uint8_t KeyboardLayout_en_GB[128];
