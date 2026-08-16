// usbdongle W3 — en_GB (UK ISO, pc105) keyboard layout table.
//
// PROVENANCE. Derived byte-for-byte from the core's KeyboardLayout_en_US.cpp
// (framework-arduinoespressif32 3.3.11,
//  libraries/USB/src/keyboardLayout/KeyboardLayout_en_US.cpp), which was read in
// full before writing this. Only SIX indices differ from en_US; every other
// entry is copied unchanged. The en_US array was the reference for the shift-
// flag encoding — it was not guessed.
//
// ENCODING (from the core's KeyboardLayout.h, verified against
// USBHIDKeyboard::press()):
//   * Each byte is a USB HID Usage ID in its low 6 bits.
//   * bit 7 (0x80, "SHIFT") -> press() adds Left-Shift.
//   * bit 6 (0x40, "ALT_GR") -> press() adds Right-Alt/AltGr. (en_GB ASCII
//     needs no AltGr, so this bit is never set here.)
//   * 0x00 means "unmapped".
//   * Because a usage must fit in 6 bits to leave bit 6 free for ALT_GR, the one
//     usage above 0x3F that a printable key needs — 0x64, Keyboard Non-US \ and
//     | (the key left of Z on ISO) — cannot be stored directly. The core stores
//     it as the sentinel 0x32 (ISO_REPLACEMENT) and press() maps 0x32 -> 0x64.
//
// THE FORMAT CANNOT REPRESENT NONUS_HASH (usage 0x32). This is the encoding
// subtlety the format hides. A UK keyboard has TWO keys the US ANSI layout does
// not:
//   * usage 0x64 — Non-US \ and |     (the key left of Z)      -> \ and |
//   * usage 0x32 — Non-US # and ~     (the key beside Enter)   -> # and ~
// The core reused table-value 0x32 as the sentinel for usage 0x64, so there is
// NO table byte left that yields usage 0x32: writing 0x32 for '#' would be
// remapped by press() to usage 0x64 and print '\'. The two keys collide in this
// format.
//
// Resolution: '\' and '|' keep the stock ISO_REPLACEMENT encoding here (so this
// table stays a valid en_US-format layout and '\'/'|' work through the ordinary
// kbd->write() path), and '#'/'~' are left UNMAPPED (0x00) in the table. The
// `hid` module emits them by pressing the raw HID usage 0x32 directly, bypassing
// the asciimap. See emitChar() in mod_hid.cpp. Do not "fix" these two to 0x32 —
// that reintroduces the collision and breaks '\'.

#include "keyboard_layout_en_gb.h"

#define KB_SHIFT 0x80

// clang-format off
const uint8_t KeyboardLayout_en_GB[128] = {
  0x00,             // NUL
  0x00,             // SOH
  0x00,             // STX
  0x00,             // ETX
  0x00,             // EOT
  0x00,             // ENQ
  0x00,             // ACK
  0x00,             // BEL
  0x2a,             // BS  Backspace
  0x2b,             // TAB Tab
  0x28,             // LF  Enter
  0x00,             // VT
  0x00,             // FF
  0x00,             // CR
  0x00,             // SO
  0x00,             // SI
  0x00,             // DEL
  0x00,             // DC1
  0x00,             // DC2
  0x00,             // DC3
  0x00,             // DC4
  0x00,             // NAK
  0x00,             // SYN
  0x00,             // ETB
  0x00,             // CAN
  0x00,             // EM
  0x00,             // SUB
  0x00,             // ESC
  0x00,             // FS
  0x00,             // GS
  0x00,             // RS
  0x00,             // US

  0x2c,             // ' '
  0x1e | KB_SHIFT,  // !
  0x1f | KB_SHIFT,  // "   GB: Shift+2  (en_US was 0x34|SHIFT)
  0x00,             // #   GB: usage 0x32 (NONUS_HASH) — UNMAPPABLE here, emitted raw by the module
  0x21 | KB_SHIFT,  // $
  0x22 | KB_SHIFT,  // %
  0x24 | KB_SHIFT,  // &
  0x34,             // '
  0x26 | KB_SHIFT,  // (
  0x27 | KB_SHIFT,  // )
  0x25 | KB_SHIFT,  // *
  0x2e | KB_SHIFT,  // +
  0x36,             // ,
  0x2d,             // -
  0x37,             // .
  0x38,             // /
  0x27,             // 0
  0x1e,             // 1
  0x1f,             // 2
  0x20,             // 3
  0x21,             // 4
  0x22,             // 5
  0x23,             // 6
  0x24,             // 7
  0x25,             // 8
  0x26,             // 9
  0x33 | KB_SHIFT,  // :
  0x33,             // ;
  0x36 | KB_SHIFT,  // <
  0x2e,             // =
  0x37 | KB_SHIFT,  // >
  0x38 | KB_SHIFT,  // ?
  0x34 | KB_SHIFT,  // @   GB: Shift+'  (en_US was 0x1f|SHIFT)
  0x04 | KB_SHIFT,  // A
  0x05 | KB_SHIFT,  // B
  0x06 | KB_SHIFT,  // C
  0x07 | KB_SHIFT,  // D
  0x08 | KB_SHIFT,  // E
  0x09 | KB_SHIFT,  // F
  0x0a | KB_SHIFT,  // G
  0x0b | KB_SHIFT,  // H
  0x0c | KB_SHIFT,  // I
  0x0d | KB_SHIFT,  // J
  0x0e | KB_SHIFT,  // K
  0x0f | KB_SHIFT,  // L
  0x10 | KB_SHIFT,  // M
  0x11 | KB_SHIFT,  // N
  0x12 | KB_SHIFT,  // O
  0x13 | KB_SHIFT,  // P
  0x14 | KB_SHIFT,  // Q
  0x15 | KB_SHIFT,  // R
  0x16 | KB_SHIFT,  // S
  0x17 | KB_SHIFT,  // T
  0x18 | KB_SHIFT,  // U
  0x19 | KB_SHIFT,  // V
  0x1a | KB_SHIFT,  // W
  0x1b | KB_SHIFT,  // X
  0x1c | KB_SHIFT,  // Y
  0x1d | KB_SHIFT,  // Z
  0x2f,             // [
  0x32,             // '\'  GB: usage 0x64 (NONUS_BACKSLASH) via ISO_REPLACEMENT sentinel 0x32 (en_US was 0x31)
  0x30,             // ]
  0x23 | KB_SHIFT,  // ^
  0x2d | KB_SHIFT,  // _
  0x35,             // `    GB: unshifted backtick (Shift+0x35 is ¬, non-ASCII, unmapped)
  0x04,             // a
  0x05,             // b
  0x06,             // c
  0x07,             // d
  0x08,             // e
  0x09,             // f
  0x0a,             // g
  0x0b,             // h
  0x0c,             // i
  0x0d,             // j
  0x0e,             // k
  0x0f,             // l
  0x10,             // m
  0x11,             // n
  0x12,             // o
  0x13,             // p
  0x14,             // q
  0x15,             // r
  0x16,             // s
  0x17,             // t
  0x18,             // u
  0x19,             // v
  0x1a,             // w
  0x1b,             // x
  0x1c,             // y
  0x1d,             // z
  0x2f | KB_SHIFT,  // {
  0x32 | KB_SHIFT,  // |    GB: Shift + usage 0x64 via ISO_REPLACEMENT (en_US was 0x31|SHIFT)
  0x30 | KB_SHIFT,  // }
  0x00,             // ~    GB: Shift + usage 0x32 (NONUS_HASH) — UNMAPPABLE here, emitted raw by the module
  0x00              // DEL
};
// clang-format on
