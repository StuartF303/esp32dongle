// usbdongle W3 — CRC-32 for the `storage` module's transfer integrity checks.
//
// DEPENDENCY-FREE ON PURPOSE, like claims.h / pathsafe.h / b64.h: <stddef.h>
// and <stdint.h> only, header-only and inline, so `pio test -e native` compiles
// and asserts it on this machine against known vectors.
//
// WHICH CRC-32. The ubiquitous one — "CRC-32/ISO-HDLC", the zlib/PNG/gzip
// variant: reflected, polynomial 0x04C11DB7 (0xEDB88320 reflected), init
// 0xFFFFFFFF, final XOR 0xFFFFFFFF. Naming it precisely matters because the
// point of the value is that a caller on the far end can compute the same
// number with `zlib.crc32`, `binascii.crc32`, `crc32` in Go or `Crc32` in .NET
// and get a match. The canonical check value, CRC32("123456789"), is
// 0xCBF43926, and that is asserted in test/test_crc32.
//
// It is INCREMENTAL because `storage.verify` hashes a whole file in bounded
// slices from the module tick and must not hold the cooperative scheduler.
// update() takes and returns the RUNNING register (pre-final-XOR); finish()
// applies the XOR exactly once, at the end. Mixing the two up is the classic
// way to get a value that is stable, plausible and wrong.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Crc32 {

// Running register before the first byte. NOT a valid CRC on its own.
constexpr uint32_t INIT = 0xFFFFFFFFu;

namespace detail {

struct Table {
  uint32_t v[256];
};

constexpr Table makeTable() {
  Table t{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    t.v[i] = c;
  }
  return t;
}

// constexpr, so it is materialised into .rodata at compile time: 1 KB of flash,
// zero RAM, no first-call initialisation and therefore no thread-safety guard.
inline constexpr Table TABLE = makeTable();

}  // namespace detail

// Folds `n` bytes into the running register. `crc` must be INIT for the first
// call and the previous return value thereafter.
inline uint32_t update(uint32_t crc, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  if (p == nullptr) {
    return crc;
  }
  for (size_t i = 0; i < n; i++) {
    crc = detail::TABLE.v[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc;
}

// Applies the final XOR. Call ONCE, on the value returned by the last update().
inline uint32_t finish(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

// One-shot convenience for a buffer that is already whole.
inline uint32_t compute(const void *data, size_t n) { return finish(update(INIT, data, n)); }

// Renders as exactly 8 lowercase hex digits, zero-padded, no "0x".
//
// The wire format is a STRING, not a number, and deliberately: 0xCBF43926 does
// not fit a signed 32-bit int, and a caller reading the response in a language
// that maps JSON numbers onto int32 (or that reformats large integers) would
// silently see a different value than the one we sent. A fixed-width hex string
// compares byte-for-byte and cannot be reinterpreted. `storage.caps` advertises
// crc32:"hex8" so the shape is discoverable.
//
// `out` must have room for 9 bytes.
// NB the digit table is NOT called HEX. Arduino's Print.h does `#define HEX 16`,
// and this header is included from translation units that pull in Arduino.h, so
// that name expands to a numeric constant and the file stops compiling. A
// dependency-free header still has to survive the macro soup of its callers.
inline void toHex8(uint32_t v, char *out) {
  static const char HEX_DIGITS[] = "0123456789abcdef";
  for (int i = 7; i >= 0; i--) {
    out[i] = HEX_DIGITS[v & 0xFu];
    v >>= 4;
  }
  out[8] = '\0';
}

}  // namespace Crc32
