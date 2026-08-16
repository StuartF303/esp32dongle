// usbdongle W1 — resource claims and the arbitration rule.
//
// DEPENDENCY-FREE ON PURPOSE. This header includes <stdint.h> and nothing
// else — no Arduino.h, no ESP-IDF, no ArduinoJson. That is what lets a native
// PlatformIO test env (`pio test -e native`) compile the arbitration logic on
// the host without any refactoring. Keep it that way: if you need Serial, a
// GPIO or a JSON document, you are in the wrong file (see registry.h).
//
// Claim model — shared/exclusive, reader-writer semantics. Decided by stuart,
// 2026-08-16. A module may be enabled iff, for every resource it claims:
//   * it wants EXCLUSIVE and no other enabled module holds that resource in
//     any mode; or
//   * it wants SHARED and no other enabled module holds that resource
//     EXCLUSIVE.
//
// Why not a plain "one owner per resource" bitmask (which is what
// ARCHITECTURE.md section 2 originally sketched): `wifiscan` and `blescan`
// genuinely can run together — Wi-Fi/BLE coexistence is real on the ESP32-S3
// and both radios share one antenna path under a coexistence arbiter — so
// both take RADIO *shared*. Wi-Fi monitor mode cannot share, so it takes
// RADIO *exclusive* and locks both of them out. `msc` hands the SD card to
// the host PC as a block device, so it takes USB and SD *exclusive* and locks
// out `hid` (USB) and `storage` (SD). A single-owner bitmask cannot express
// the first case without lying about the second.

#pragma once

#include <stdint.h>

namespace Claims {

// Physical/logical resources modules contend for. Extend by adding before
// RES_COUNT; RESOURCE_NAMES below must be kept in the same order (there is a
// static_assert-style guard in resourceName()).
enum Resource : uint8_t {
  RES_USB = 0,   // the OTG/CDC peripheral and its descriptor set
  RES_SD,        // the SDMMC 4-bit bus (GPIO 12/16/14/17/21/18)
  RES_RADIO,     // Wi-Fi + BLE, arbitrated together — they share one antenna
  RES_LCD,       // ST7735 on SPI2_HOST (GPIO 3/5/4/2/1, backlight 38)
  RES_LED,       // APA102 on GPIO 40 (data) / 39 (clock)
  RES_COUNT
};

enum Mode : uint8_t {
  CLAIM_NONE = 0,       // module does not touch this resource
  CLAIM_SHARED = 1,     // concurrent use is safe with other SHARED holders
  CLAIM_EXCLUSIVE = 2,  // sole use required
};

// One module's claims over every resource. Fixed size, no allocation — this
// chip has no PSRAM and 320 KB usable RAM is a real budget.
struct ClaimSet {
  Mode mode[RES_COUNT];
};

// The whole arbitration rule, in one place.
constexpr bool modesCompatible(Mode a, Mode b) {
  return a == CLAIM_NONE || b == CLAIM_NONE || (a == CLAIM_SHARED && b == CLAIM_SHARED);
}

// Index of the first resource on which `a` and `b` conflict, or RES_COUNT if
// they can be enabled at the same time. Returning the resource (not just a
// bool) is what lets the registry say *why* it refused.
constexpr uint8_t firstConflict(const ClaimSet &a, const ClaimSet &b) {
  uint8_t r = 0;
  for (; r < RES_COUNT; r++) {
    if (!modesCompatible(a.mode[r], b.mode[r])) {
      break;
    }
  }
  return r;
}

constexpr bool coexist(const ClaimSet &a, const ClaimSet &b) {
  return firstConflict(a, b) == RES_COUNT;
}

// ---- builders ----------------------------------------------------------
//
// Positional aggregate init ({{CLAIM_NONE, CLAIM_NONE, ...}}) would silently
// break the moment someone inserts a resource into the enum, so module
// descriptors use these instead and never depend on the enum's order.

constexpr ClaimSet none() {
  ClaimSet c{};
  for (uint8_t r = 0; r < RES_COUNT; r++) {
    c.mode[r] = CLAIM_NONE;
  }
  return c;
}

constexpr ClaimSet claim(Resource r1, Mode m1) {
  ClaimSet c = none();
  c.mode[r1] = m1;
  return c;
}

constexpr ClaimSet claim(Resource r1, Mode m1, Resource r2, Mode m2) {
  ClaimSet c = claim(r1, m1);
  c.mode[r2] = m2;
  return c;
}

constexpr ClaimSet claim(Resource r1, Mode m1, Resource r2, Mode m2, Resource r3, Mode m3) {
  ClaimSet c = claim(r1, m1, r2, m2);
  c.mode[r3] = m3;
  return c;
}

// ---- names (wire-visible; keep them short and stable) -------------------

inline const char *resourceName(uint8_t r) {
  switch (r) {
    case RES_USB:
      return "usb";
    case RES_SD:
      return "sd";
    case RES_RADIO:
      return "radio";
    case RES_LCD:
      return "lcd";
    case RES_LED:
      return "led";
    default:
      return "?";
  }
}

inline const char *modeName(Mode m) {
  switch (m) {
    case CLAIM_SHARED:
      return "shared";
    case CLAIM_EXCLUSIVE:
      return "exclusive";
    default:
      return "none";
  }
}

}  // namespace Claims
