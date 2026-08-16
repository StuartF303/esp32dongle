// usbdongle W3 — base64 for the `storage` module's chunked file transfer.
//
// DEPENDENCY-FREE ON PURPOSE, exactly like claims.h and pathsafe.h:
// <stddef.h>/<stdint.h> and nothing else, header-only and inline, so
// `pio test -e native` can compile and assert it on this machine. The native
// env sets build_src_filter = -<*>, so a .cpp here would never be built for the
// host and the codec would go untested. Keep it that way.
//
// WHY IT IS STRICT. The decoder's output goes straight onto the SD card. A
// lenient decoder — one that skips whitespace, tolerates missing padding, or
// ignores the leftover bits of a partial group — accepts several distinct
// inputs for the same output and, worse, accepts inputs that were *corrupted in
// transit* and writes the corruption to the card as if it were fine. The
// caller-visible contract is a crc32 of the decoded bytes, and that contract is
// worth nothing if the decode step silently repairs its input.
//
// So this decoder rejects, and names the reason:
//   * length not a multiple of 4                -> B64_BAD_LENGTH
//   * any byte outside A-Za-z0-9+/= (INCLUDING
//     whitespace and newlines — a JSON string
//     has no business containing them)          -> B64_BAD_CHAR
//   * '=' anywhere but the last one or two
//     positions of the final group              -> B64_BAD_PADDING
//   * non-zero leftover bits in a padded group
//     (e.g. "AB==", which is "AA==" corrupted)  -> B64_NON_CANONICAL
//   * output longer than the caller's buffer    -> B64_OVERFLOW
//
// It is standard base64 (RFC 4648 §4), NOT the URL-safe alphabet: '+' and '/',
// with '=' padding always present. `storage.caps` advertises encoding:"base64"
// so a caller never has to guess which variant.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace B64 {

// Encoded length of `rawLen` bytes, EXCLUDING the NUL. Always a multiple of 4
// because padding is always emitted.
constexpr size_t encodedLen(size_t rawLen) { return ((rawLen + 2) / 3) * 4; }

// Upper bound on the decoded length of `encLen` base64 characters. The true
// length is 1 or 2 less when the input is padded; decode() reports the exact
// value.
constexpr size_t maxDecodedLen(size_t encLen) { return (encLen / 4) * 3; }

enum Result : uint8_t {
  B64_OK = 0,
  B64_NULL,           // null src/dst
  B64_BAD_LENGTH,     // length is not a multiple of 4
  B64_BAD_CHAR,       // a byte outside the alphabet (whitespace included)
  B64_BAD_PADDING,    // '=' outside the tail of the final group
  B64_NON_CANONICAL,  // padded group carries bits that a re-encode would drop
  B64_OVERFLOW,       // decoded output does not fit the caller's buffer
};

constexpr char ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Value of one base64 character, or -1 if it is not in the alphabet. A branch
// chain rather than a 256-byte lookup table: this is not the hot path (SD I/O
// is), and 256 bytes of .rodata per image is worth more than the few cycles.
inline int decodeChar(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

// Encodes `n` bytes into `dst`, NUL-terminated. Returns the number of base64
// characters written, or 0 if `dst` cannot hold encodedLen(n) + 1 bytes —
// never a partial write, so a caller that ignores the return value gets an
// empty string rather than a truncated one that looks valid.
inline size_t encode(const uint8_t *src, size_t n, char *dst, size_t dstCap) {
  if (dst == nullptr || dstCap == 0) {
    return 0;
  }
  dst[0] = '\0';
  if (src == nullptr && n > 0) {
    return 0;
  }
  if (dstCap < encodedLen(n) + 1) {
    return 0;
  }

  size_t o = 0;
  size_t i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | (uint32_t)src[i + 2];
    dst[o++] = ENC[(v >> 18) & 63];
    dst[o++] = ENC[(v >> 12) & 63];
    dst[o++] = ENC[(v >> 6) & 63];
    dst[o++] = ENC[v & 63];
  }
  size_t rem = n - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)src[i] << 16;
    dst[o++] = ENC[(v >> 18) & 63];
    dst[o++] = ENC[(v >> 12) & 63];
    dst[o++] = '=';
    dst[o++] = '=';
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
    dst[o++] = ENC[(v >> 18) & 63];
    dst[o++] = ENC[(v >> 12) & 63];
    dst[o++] = ENC[(v >> 6) & 63];
    dst[o++] = '=';
  }
  dst[o] = '\0';
  return o;
}

// Decodes exactly `srcLen` characters into `dst`, writing the byte count to
// `outLen`. Returns B64_OK only for canonical, fully-padded, in-alphabet input
// that fits.
//
// BOUND-CHECKED BEFORE THE FIRST BYTE IS WRITTEN: the output length is computed
// from the input length and the padding and compared against `dstCap` up front,
// so an over-long input cannot write a single byte past the buffer, and a
// rejected input never leaves partial garbage behind for a caller to write to
// the card.
inline Result decode(const char *src, size_t srcLen, uint8_t *dst, size_t dstCap, size_t *outLen) {
  if (outLen != nullptr) {
    *outLen = 0;
  }
  if (src == nullptr) {
    return B64_NULL;
  }
  if (srcLen == 0) {
    return B64_OK;  // an empty payload decodes to zero bytes; that is legal
  }
  if ((srcLen & 3u) != 0) {
    return B64_BAD_LENGTH;
  }
  if (dst == nullptr) {
    return B64_NULL;
  }

  // Padding count from the tail, used ONLY to size the output. Its legality is
  // still checked per-group below, so "=A==" is rejected rather than sized.
  size_t pad = 0;
  if (src[srcLen - 1] == '=') {
    pad = (src[srcLen - 2] == '=') ? 2 : 1;
  }
  size_t need = (srcLen / 4) * 3 - pad;
  if (need > dstCap) {
    return B64_OVERFLOW;
  }

  size_t o = 0;
  for (size_t i = 0; i < srcLen; i += 4) {
    bool last = (i + 4 == srcLen);
    int q[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; k++) {
      char c = src[i + k];
      if (c == '=') {
        // Padding is legal only in the last group, and only in positions 2/3.
        if (!last || k < 2) {
          return B64_BAD_PADDING;
        }
        if (k == 2 && src[i + 3] != '=') {
          return B64_BAD_PADDING;  // "AB=C": a symbol after a pad
        }
        q[k] = -1;
        continue;
      }
      if (k > 0 && q[k - 1] == -1) {
        return B64_BAD_PADDING;  // a symbol following a pad character
      }
      int v = decodeChar(c);
      if (v < 0) {
        return B64_BAD_CHAR;
      }
      q[k] = v;
    }

    if (q[1] < 0) {
      return B64_BAD_PADDING;  // "A===" — a group needs at least two symbols
    }
    if (q[2] < 0) {
      // 2 symbols -> 1 byte. The low 4 bits of the second symbol are dropped by
      // a re-encode, so a non-zero value there means the input is not what any
      // encoder produced.
      if ((q[1] & 0x0F) != 0) {
        return B64_NON_CANONICAL;
      }
      dst[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
    } else if (q[3] < 0) {
      if ((q[2] & 0x03) != 0) {  // same argument, low 2 bits of the third symbol
        return B64_NON_CANONICAL;
      }
      dst[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
      dst[o++] = (uint8_t)(((q[1] & 0x0F) << 4) | (q[2] >> 2));
    } else {
      dst[o++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
      dst[o++] = (uint8_t)(((q[1] & 0x0F) << 4) | (q[2] >> 2));
      dst[o++] = (uint8_t)(((q[2] & 0x03) << 6) | q[3]);
    }
  }

  if (outLen != nullptr) {
    *outLen = o;
  }
  return B64_OK;
}

// Short, stable, wire-visible token — goes into the error response's `d` so a
// caller can branch on the reason without parsing prose.
inline const char *resultName(Result r) {
  switch (r) {
    case B64_OK:
      return "ok";
    case B64_NULL:
      return "null";
    case B64_BAD_LENGTH:
      return "bad_length";
    case B64_BAD_CHAR:
      return "bad_char";
    case B64_BAD_PADDING:
      return "bad_padding";
    case B64_NON_CANONICAL:
      return "non_canonical";
    case B64_OVERFLOW:
      return "overflow";
    default:
      return "?";
  }
}

// One line, actionable, for a human. Never a bare "bad base64".
inline const char *resultMessage(Result r) {
  switch (r) {
    case B64_OK:
      return "ok";
    case B64_NULL:
      return "missing p.data";
    case B64_BAD_LENGTH:
      return "p.data length is not a multiple of 4; base64 here is always padded with '='";
    case B64_BAD_CHAR:
      return "p.data has a character outside A-Za-z0-9+/=; whitespace and newlines are not accepted, and the URL-safe "
             "alphabet ('-' and '_') is not accepted";
    case B64_BAD_PADDING:
      return "p.data has '=' somewhere other than the last one or two characters";
    case B64_NON_CANONICAL:
      return "p.data's final group carries bits that a re-encode would drop, so it is not what an encoder produced — "
             "the payload is corrupt, and it was NOT written";
    case B64_OVERFLOW:
      return "p.data decodes to more bytes than max_chunk (see the storage caps action)";
    default:
      return "p.data is not acceptable base64";
  }
}

}  // namespace B64
