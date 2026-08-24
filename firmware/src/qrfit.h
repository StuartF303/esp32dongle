// usbdongle W3 — "will this QR actually be readable on an 80-pixel panel?"
//
// One decision, isolated so it can be tested on this machine:
//
//     payload -> does it encode at all? -> which version -> which pixel scale
//               -> draw the QR, or fall back to text
//
// ---- WHY THE ANSWER IS BOUNDED, AND BY WHAT ----------------------------
//
// ARCHITECTURE.md §"QR pairing on the LCD" measured this rather than
// estimating it. Two requirements do all the work:
//
//   * A SPEC-COMPLIANT QUIET ZONE — 4 clear modules on every side. Not
//     negotiable in the way it is tempting to think it is: the quiet zone is
//     how a decoder finds the symbol's edge at all, and this panel is a bright
//     rectangle on a dark plastic dongle with nothing else white near it.
//   * AT LEAST 2 PHYSICAL PIXELS PER MODULE. The panel pitch is 0.136 mm
//     (0.96" diagonal, 178.9 px across it), so 2 px is a 0.27 mm module, which
//     a 12 MP phone at 10 cm resolves at roughly 8 camera pixels — comfortably
//     above the ~3 px a decoder needs. One pixel per module is 0.14 mm and is
//     below what a phone can resolve at any sane distance.
//
// Together they close the problem:
//
//     scale = 80 / (size + 8)   (integer division; 8 == 2 x 4 quiet modules)
//     scale >= 2  <=>  size <= 32  <=>  version <= 3 (29x29)
//
// So VERSION 3 IS THE LARGEST CODE THIS FIRMWARE CAN EVER DRAW, and that is a
// static_assert below rather than a comment: version 4 is 33 modules, which is
// 41 with the quiet zone, which is scale 1. MAX_VERSION is also passed to
// qrcodegen_encodeText() as maxVersion, so an over-long payload fails IN THE
// ENCODER — returning false — instead of producing a version 5 symbol that
// this file would then have to refuse to draw. Failing early keeps the two
// buffers at qrcodegen_BUFFER_LEN_FOR_VERSION(3) == 107 bytes each, about
// 214 bytes of stack for the largest code, with no heap and no PSRAM.
//
// ---- THE FALLBACK IS A REAL CASE, NOT DEFENSIVE CODE -------------------
//
// `psk set` accepts any legal WPA2 passphrase up to 63 characters (mod_http.cpp
// documents why that exists and what it costs). Measured through this same
// encoder, a `WIFI:` payload carrying a 63-character passphrase is 93
// characters and needs version 5 — 37 modules, 45 with the quiet zone, 1 px
// per module at 80 px of panel. So it does not encode here at all, ok() is
// false, and the renderer prints the SSID and passphrase as text instead.
//
// AN UNREADABLE QR IS WORSE THAN NO QR, because it looks like it should work:
// the user stands there re-aiming a camera at a symbol that can never resolve,
// instead of reading four lines of text and typing them.
//
// ---- DEPENDENCY-FREE IN THE SAME SENSE AS ITS NEIGHBOURS ---------------
//
// Same rule as apgrace.h / pinpolicy.h / ratelimit.h: header-only, no Arduino,
// no FreeRTOS, so `pio test -e native` drives it on this machine —
// build_src_filter = -<*> means anything living in mod_display.cpp cannot be
// host-tested at all. It DOES include <qrcodegen.h>, which is the point:
// firmware/lib/ is compiled into the native env too, so the host suite asserts
// against real encoder output — actual versions for the actual payload strings
// — rather than against a model of what the encoder is assumed to do.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <qrcodegen.h>

namespace QrFit {

// The square block the code has to live inside. It is the PANEL HEIGHT: a
// 160x80 panel constrains a square symbol by its short side, and the renderer
// gives the QR an 80x80 block on the left with the digits beside it.
constexpr int BLOCK_PX = 80;

// Spec quiet zone, per side, in modules.
constexpr int QUIET_MODULES = 4;

// The floor that makes the whole thing decidable. See the header comment.
constexpr int MIN_SCALE = 2;

// Largest version that can be drawn at MIN_SCALE. Passed to the encoder as
// maxVersion, so this constant is enforced twice: once by refusing to encode
// anything bigger, once by the static_asserts below.
constexpr int MAX_VERSION = 3;

// Both buffers the encoder wants, sized from the version cap.
constexpr size_t BUF_LEN = (size_t)qrcodegen_BUFFER_LEN_FOR_VERSION(MAX_VERSION);
static_assert(BUF_LEN == 107, "qrcodegen_BUFFER_LEN_FOR_VERSION(3) is no longer 107 bytes");
static_assert(2 * BUF_LEN <= 256, "the scratch+output pair no longer fits the ~214 byte stack budget");

// Modules per side for a version. 21, 25, 29 for versions 1..3.
constexpr int sizeForVersion(int version) { return version * 4 + 17; }

// Physical pixels per module, or 0 if the code cannot be drawn at MIN_SCALE.
// Integer division on purpose: a fractional scale would put module boundaries
// between pixels, which is exactly the smearing this is trying to avoid.
constexpr int scaleFor(int size) {
  if (size <= 0) {
    return 0;
  }
  int s = BLOCK_PX / (size + 2 * QUIET_MODULES);
  return s >= MIN_SCALE ? s : 0;
}

// Total drawn extent, quiet zone included, in pixels.
constexpr int extentFor(int size) { return (size + 2 * QUIET_MODULES) * scaleFor(size); }

// THE VERSION CAP, PROVEN RATHER THAN ASSERTED IN PROSE.
static_assert(scaleFor(sizeForVersion(1)) == 2, "version 1 (21 modules) must draw at 2 px/module");
static_assert(scaleFor(sizeForVersion(2)) == 2, "version 2 (25 modules) must draw at 2 px/module");
static_assert(scaleFor(sizeForVersion(3)) == 2, "version 3 (29 modules) must draw at 2 px/module");
static_assert(scaleFor(sizeForVersion(4)) == 0, "version 4 would need 41 px of quiet-zoned width per 80; MAX_VERSION is wrong");
static_assert(extentFor(sizeForVersion(1)) == 58, "version 1 extent moved");
static_assert(extentFor(sizeForVersion(3)) == 74, "version 3 extent moved");
static_assert(extentFor(sizeForVersion(MAX_VERSION)) <= BLOCK_PX, "the largest drawable code no longer fits the block");

// ECC LOW with boostEcl: LOW is what makes the pair URL fit in version 1, and
// boostEcl then raises the level for free whenever the chosen version has room
// to spare. This is the combination ARCHITECTURE.md's measured table was
// produced with; changing either invalidates that table.
constexpr enum qrcodegen_Ecc ECC = qrcodegen_Ecc_LOW;
constexpr bool BOOST_ECL = true;

struct Fit {
  bool ok;      // a symbol was produced AND it can be drawn at >= MIN_SCALE
  int size;     // modules per side, 0 when !ok
  int scale;    // physical pixels per module, 0 when !ok
  int extent;   // size+quiet, in pixels — what the renderer actually paints
  int offset;   // pixels from the block edge to the quiet zone's outer edge
};

// The geometry half, with no encoder involved: given a module count, say how
// it would be drawn. Pure, so the whole table in ARCHITECTURE.md is testable
// without encoding anything.
constexpr Fit fitFor(int size) {
  int scale = scaleFor(size);
  if (scale == 0) {
    return Fit{false, 0, 0, 0, 0};
  }
  int extent = (size + 2 * QUIET_MODULES) * scale;
  return Fit{true, size, scale, extent, (BLOCK_PX - extent) / 2};
}

// Encode and decide, in one call.
//
// `tmp` and `qr` must each be BUF_LEN bytes. On success `qr` holds the symbol
// and the caller reads it with qrcodegen_getModule(); on failure `qr` holds
// nothing useful and the caller must draw the text fallback.
//
// Returns ok == false for: a null/empty payload, a payload that will not
// encode within MAX_VERSION, or (unreachable while MAX_VERSION is 3, and
// asserted above) a symbol too large to draw at MIN_SCALE.
inline Fit encode(const char *text, uint8_t *tmp, uint8_t *qr) {
  Fit none{false, 0, 0, 0, 0};
  if (text == nullptr || text[0] == '\0' || tmp == nullptr || qr == nullptr) {
    return none;
  }
  if (!qrcodegen_encodeText(text, tmp, qr, ECC, qrcodegen_VERSION_MIN, MAX_VERSION, qrcodegen_Mask_AUTO, BOOST_ECL)) {
    return none;
  }
  return fitFor(qrcodegen_getSize(qr));
}

}  // namespace QrFit
