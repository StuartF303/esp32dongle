// usbdongle W2 — secret formats for the HTTP transport: the AP passphrase, the
// pairing PIN, and session tokens.
//
// DEPENDENCY-FREE ON PURPOSE, like ct.h / b64.h / pathsafe.h — <stddef.h>,
// <stdint.h>, <string.h> and nothing else, header-only and inline, so
// `pio test -e native` compiles and asserts it on this machine. The generators
// take the RNG as a function pointer for exactly that reason: the device passes
// esp_fill_random, a test passes a counter, and the *format* logic (bias,
// alphabet, length, termination) is then testable without a radio.
//
// WHERE THE ENTROPY COMES FROM IS THE CALLER'S PROBLEM, and it is the whole
// ballgame — see mod_http.cpp. esp_random() is only a true TRNG once the RF
// subsystem is running; before that it is a much weaker source. Nothing in this
// file can rescue a predictable RandomBytesFn.
//
// ---- bias --------------------------------------------------------------
//
// One technique, applied to both alphabets, because neither divides 256:
//
//   * PSK: 31-character alphabet. Bytes >= 248 (248 == 8 * 31) are REJECTED
//     and redrawn; 0..247 map onto 0..30 exactly evenly. Rejection rate 8/256.
//   * PIN: 10 digits. Bytes >= 250 (250 == 25 * 10) are REJECTED and redrawn.
//     Rejection rate 6/256.
//
// `byte % 31` or `byte % 10` on their own would make the low symbols of each
// alphabet more likely than the high ones. Rejection sampling is exact; the
// redraw is rare and bounded in both cases.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace AuthFmt {

// ---- formats -------------------------------------------------------------

// 4 digits: 10^4 combinations. SAY IT PLAINLY — 4 digits ALONE WOULD BE
// INDEFENSIBLE. 10,000 is a space a script walks in seconds if it is allowed to
// walk it, and nothing in this header makes that better. Stuart's decision,
// 2026-08-24 (ARCHITECTURE.md, "Pairing model"); what makes it hold is three
// other properties, none of which live here:
//
//   * THE LIMITER (ratelimit.h). 1, 2, 4, 8, 16, 30, 30... seconds between
//     guesses, then a 15-minute lockout at 10 failures: about 34 guesses an
//     hour. Against a STATIC 4-digit PIN that is still only ~6 days to walk
//     10^4 — a bounded search, and the bounded part is the objectionable part.
//   * SINGLE-USE ROTATION, which is what removes the bound. mod_http.cpp mints
//     a fresh PIN on power-up, ON USE, on session end, AND on every lockout, so
//     each 17.5-minute cycle spends its 10 guesses against a FRESH 10^4 space.
//     The attacker accumulates no progress at all; expected effort stops being
//     a search and becomes an unbounded sequence of independent 1-in-1000
//     draws. "On use" is what makes single-use literal rather than a figure of
//     speech: a PIN that has opened a session cannot open a second one.
//   * SINGLE-CLIENT ASSOCIATION. AP_MAX_CLIENTS is 1, so while the owner's
//     phone holds the slot nobody else can associate to guess at all. The
//     brute-force window exists only while the device is unpaired.
//
// So the PIN length is not the control; the rotation and the limiter are, and
// removing either one turns this constant into a real weakness rather than a
// tolerable one. If PIN_LEN ever grows again, note that nothing here depends on
// it being 4 — the buffers and the display budget are all derived.
//
// WHAT THE FOUR DIGITS BUY: glyph height. The PIN is read off a 160x80 ST7735,
// usually at arm's length, from a dongle in the front of a desktop machine, in
// whatever light the room has. Halving the character count roughly doubles the
// height each character can be drawn at. That is the entire gain, and it is a
// legibility gain, not a security one.
constexpr size_t PIN_LEN = 4;

// ---- WPA2 passphrase bounds ----------------------------------------------
//
// IEEE 802.11i: a WPA2-PSK *passphrase* is 8..63 characters, each in the
// printable ASCII range 0x20..0x7e, and is stretched to the 256-bit PMK by
// PBKDF2. A 64-character value is NOT a passphrase — it is the raw PMK written
// as hex, a different input to a different code path. This firmware accepts
// passphrases only, so 64 is rejected as too long rather than half-supported.
constexpr size_t PSK_MIN = 8;
constexpr size_t PSK_MAX = 63;
constexpr char PSK_CHAR_MIN = 0x20;  // space
constexpr char PSK_CHAR_MAX = 0x7e;  // tilde

// How far checkPassphrase() will walk an unterminated or absurd input before
// giving up and calling it too long. Generous enough that the reported length
// is the real one for anything a human or a fat-fingered client would send.
constexpr size_t PSK_SCAN_CAP = 256;
static_assert(PSK_SCAN_CAP > PSK_MAX + 1, "the scan cap must be able to prove a value is over-length");

// ---- generated passphrase ------------------------------------------------
//
// 13 symbols drawn from a 31-character alphabet: log2(31) * 13 = 64.4 bits.
// Rendered in 4-4-5 groups separated by '-', so the stored/typed string is 15
// characters: "abcd-efgh-jkmnp".
//
// The old form was 20 symbols from a 32-character alphabet (100 bits) in one
// unbroken run, and it was unusable: it got mistyped into a phone and the WPA2
// association failed. 64.4 bits is far beyond what an offline PBKDF2 attack on
// a 4-way handshake can reach, and the grouping is what makes the string
// retypeable off a 160x80 screen.
constexpr size_t PSK_GEN_SYMBOLS = 13;
constexpr size_t PSK_GEN_LEN = 15;  // 13 symbols + 2 group separators
constexpr char PSK_GROUP_SEP = '-';
static_assert(PSK_GEN_LEN >= PSK_MIN && PSK_GEN_LEN <= PSK_MAX, "the generated form must be a legal WPA2 passphrase");

// 24 bytes = 192 bits, rendered as 48 lowercase hex characters. The floor asked
// for was 128 bits; 192 costs 16 more header bytes per request and removes the
// question entirely.
constexpr size_t TOKEN_BYTES = 24;
constexpr size_t TOKEN_LEN = TOKEN_BYTES * 2;
static_assert(TOKEN_BYTES * 8 >= 128, "session tokens must carry at least 128 bits");

// Lowercase letters and digits with every ambiguous glyph removed: no 0/o/O,
// no 1/l/I. 8 digits (2-9) + 23 letters (a-z less i, l, o) = 31. This string
// gets read off a 160x80 screen and typed into a phone by hand, so a character
// that can be misread is a character that causes a failed association.
//
// '-' is deliberately NOT in the alphabet: it is the group separator, and
// keeping the two disjoint is what lets isGeneratedPsk() check the shape
// without ambiguity.
constexpr char PSK_ALPHABET[] = "23456789abcdefghjkmnpqrstuvwxyz";
constexpr size_t PSK_ALPHABET_LEN = sizeof(PSK_ALPHABET) - 1;
static_assert(PSK_ALPHABET_LEN == 31, "PSK_ALPHABET must be 31 characters; the rejection bound below assumes it");
// 248 == 8 * 31: bytes 0..247 map onto 0..30 evenly, 248..255 are redrawn.
constexpr uint8_t PSK_REJECT_AT = 248;
static_assert(PSK_REJECT_AT == (256u / 31u) * 31u, "PSK_REJECT_AT must be the largest multiple of the alphabet size");

// ---- RNG injection -------------------------------------------------------

// Fills `out` with `n` random bytes. MUST be a CSPRNG/TRNG on the device.
typedef void (*RandomBytesFn)(uint8_t *out, size_t n);

// ---- generators ----------------------------------------------------------
//
// All three write a NUL-terminated string and return false (writing "") if the
// buffer is too small, so a caller that ignores the result never ships a
// truncated secret.

// Writes the 4-4-5 grouped form, e.g. "abcd-efgh-jkmnp" — PSK_GEN_LEN
// characters plus a NUL. 64.4 bits of entropy (see PSK_GEN_SYMBOLS).
inline bool makePsk(char *out, size_t cap, RandomBytesFn rng) {
  if (out == nullptr || rng == nullptr || cap < PSK_GEN_LEN + 1) {
    if (out != nullptr && cap > 0) {
      out[0] = '\0';
    }
    return false;
  }
  char sym[PSK_GEN_SYMBOLS];
  size_t got = 0;
  // Bounded, for the same reason makePin() is bounded: 8 refills of 32 bytes is
  // 256 draws for 13 symbols. Exhausting that means the RNG is returning >= 248
  // essentially always, i.e. it is broken — and a broken RNG must fail loudly,
  // not quietly fall back to `% 31`.
  for (uint8_t refill = 0; refill < 8 && got < PSK_GEN_SYMBOLS; refill++) {
    uint8_t raw[32];
    rng(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw) && got < PSK_GEN_SYMBOLS; i++) {
      if (raw[i] < PSK_REJECT_AT) {
        sym[got++] = PSK_ALPHABET[raw[i] % PSK_ALPHABET_LEN];
      }
    }
    memset(raw, 0, sizeof(raw));
  }
  if (got < PSK_GEN_SYMBOLS) {
    out[0] = '\0';
    memset(sym, 0, sizeof(sym));
    return false;
  }

  // 4-4-5, separators at index 4 and 9.
  size_t o = 0, s = 0;
  for (size_t group = 0; group < 3; group++) {
    size_t n = (group == 2) ? 5u : 4u;
    if (group != 0) {
      out[o++] = PSK_GROUP_SEP;
    }
    for (size_t i = 0; i < n; i++) {
      out[o++] = sym[s++];
    }
  }
  out[o] = '\0';
  memset(sym, 0, sizeof(sym));
  return o == PSK_GEN_LEN && s == PSK_GEN_SYMBOLS;
}

inline bool makePin(char *out, size_t cap, RandomBytesFn rng) {
  if (out == nullptr || rng == nullptr || cap < PIN_LEN + 1) {
    if (out != nullptr && cap > 0) {
      out[0] = '\0';
    }
    return false;
  }
  // Rejection sampling. 250 == 25 * 10, so bytes 0..249 map onto 0..9 exactly
  // evenly and 250..255 are thrown away.
  size_t got = 0;
  // Bounded: 8 refills of 32 bytes is 256 draws for PIN_LEN digits. Reaching
  // the end of that means the RNG is returning >= 250 essentially always, i.e.
  // it is broken — and a broken RNG must fail loudly, not fall back to `% 10`.
  for (uint8_t refill = 0; refill < 8 && got < PIN_LEN; refill++) {
    uint8_t raw[32];
    rng(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw) && got < PIN_LEN; i++) {
      if (raw[i] < 250u) {
        out[got++] = (char)('0' + (raw[i] % 10u));
      }
    }
    memset(raw, 0, sizeof(raw));
  }
  if (got < PIN_LEN) {
    out[0] = '\0';
    return false;
  }
  out[PIN_LEN] = '\0';
  return true;
}

inline bool makeToken(char *out, size_t cap, RandomBytesFn rng) {
  if (out == nullptr || rng == nullptr || cap < TOKEN_LEN + 1) {
    if (out != nullptr && cap > 0) {
      out[0] = '\0';
    }
    return false;
  }
  // NOT named HEX: Arduino's Print.h defines HEX as a macro (16), and this
  // header is included after Arduino.h on the device.
  static const char HEX_DIGITS[] = "0123456789abcdef";
  uint8_t raw[TOKEN_BYTES];
  rng(raw, sizeof(raw));
  for (size_t i = 0; i < TOKEN_BYTES; i++) {
    out[i * 2] = HEX_DIGITS[(raw[i] >> 4) & 0x0f];
    out[i * 2 + 1] = HEX_DIGITS[raw[i] & 0x0f];
  }
  out[TOKEN_LEN] = '\0';
  memset(raw, 0, sizeof(raw));
  return true;
}

// ---- validators ----------------------------------------------------------
//
// Used on BOTH sides: to sanity-check what came out of NVS (a corrupted or
// half-written secret must be regenerated, not used) and to reject malformed
// input before it reaches a comparison.

inline bool validPin(const char *s) {
  // Bound is LEN+1, not LEN+2: strnlen(s, LEN+1) == LEN proves the string is
  // EXACTLY LEN, since anything longer returns LEN+1. LEN+2 would also read one
  // byte past a tightly-sized LEN+1 buffer, which -Wstringop-overread catches.
  if (s == nullptr || strnlen(s, PIN_LEN + 1) != PIN_LEN) {
    return false;
  }
  for (size_t i = 0; i < PIN_LEN; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return true;
}

// Why a passphrase was rejected. The caller turns this into a message naming
// the ACTUAL length and the limit — "invalid" on its own is what makes a user
// try the same wrong thing twice.
enum PassphraseCheck : uint8_t {
  PASSPHRASE_OK = 0,
  PASSPHRASE_NULL = 1,
  PASSPHRASE_SHORT = 2,     // < PSK_MIN
  PASSPHRASE_LONG = 3,      // > PSK_MAX (64 exactly is the raw-hex-PMK case)
  PASSPHRASE_BAD_CHAR = 4,  // outside printable ASCII 0x20..0x7e
};

// The WPA2 rule and nothing more: 8..63 characters, each printable ASCII.
// `lenOut` gets the measured length (capped at `scanCap`) and `badOut` the
// index of the first offending byte, both optional.
//
// DELIBERATELY PERMISSIVE about content. This validates an owner-chosen
// passphrase as well as one of ours, so it cannot demand the generated
// alphabet. Use isGeneratedPsk() when the question is "did we mint this?".
//
// `scanCap` MUST NOT EXCEED THE SIZE OF THE BUFFER `s` POINTS AT. The default
// suits a NUL-terminated string inside a large parse buffer, where over-length
// input should still be measured so the error can name its real length. A
// caller validating a tightly-sized array must pass sizeof(that array) instead,
// or -Wstringop-overread will (correctly) object. PSK_MAX + 1 is the smallest
// cap that still tells 63 from 64, so anything from there up behaves the same
// except in how large a length it can report.
inline PassphraseCheck checkPassphrase(const char *s, size_t *lenOut, size_t *badOut,
                                       size_t scanCap = PSK_SCAN_CAP) {
  if (lenOut != nullptr) {
    *lenOut = 0;
  }
  if (badOut != nullptr) {
    *badOut = 0;
  }
  if (s == nullptr) {
    return PASSPHRASE_NULL;
  }
  size_t len = strnlen(s, scanCap);
  if (lenOut != nullptr) {
    *lenOut = len;
  }
  if (len < PSK_MIN) {
    return PASSPHRASE_SHORT;
  }
  if (len > PSK_MAX) {
    return PASSPHRASE_LONG;
  }
  for (size_t i = 0; i < len; i++) {
    // Signed char on xtensa and x86 alike, so a UTF-8 continuation byte reads
    // negative — compare through unsigned char and every non-ASCII byte,
    // control character and DEL is caught by the one test.
    unsigned char c = (unsigned char)s[i];
    if (c < (unsigned char)PSK_CHAR_MIN || c > (unsigned char)PSK_CHAR_MAX) {
      if (badOut != nullptr) {
        *badOut = i;
      }
      return PASSPHRASE_BAD_CHAR;
    }
  }
  return PASSPHRASE_OK;
}

inline bool validPassphrase(const char *s, size_t scanCap = PSK_SCAN_CAP) {
  return checkPassphrase(s, nullptr, nullptr, scanCap) == PASSPHRASE_OK;
}

// True iff `s` has the exact shape makePsk() produces. Used only to answer
// "generated or owner-chosen?" as a cross-check; the authoritative answer is
// the flag persisted alongside the passphrase in NVS, because an owner is free
// to type something that happens to look generated.
inline bool isGeneratedPsk(const char *s) {
  if (s == nullptr || strnlen(s, PSK_GEN_LEN + 1) != PSK_GEN_LEN) {
    return false;
  }
  for (size_t i = 0; i < PSK_GEN_LEN; i++) {
    bool sep = (i == 4 || i == 9);
    if (sep) {
      if (s[i] != PSK_GROUP_SEP) {
        return false;
      }
    } else if (strchr(PSK_ALPHABET, s[i]) == nullptr) {
      // strchr() also matches the terminator, but the strnlen() above already
      // proved there is no NUL inside the first PSK_GEN_LEN bytes.
      return false;
    }
  }
  return true;
}

inline bool validToken(const char *s) {
  if (s == nullptr || strnlen(s, TOKEN_LEN + 1) != TOKEN_LEN) {
    return false;
  }
  for (size_t i = 0; i < TOKEN_LEN; i++) {
    char c = s[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) {
      return false;  // lowercase only: one spelling per token, so a lookup cannot miss
    }
  }
  return true;
}

// ---- Authorization header ------------------------------------------------
//
// Extracts the token from `Authorization: Bearer <token>`. RFC 6750 says the
// scheme is case-insensitive; the token itself is not.
//
// Strict about everything else on purpose — leading spaces, a second parameter,
// a comma-separated credential list, or any non-hex byte is a REJECT rather
// than something to be salvaged. This runs before authentication, on input from
// an unauthenticated stranger, so it is the one place where being generous is
// pure downside. Returns false and writes "" on anything it does not like.
inline bool bearerToken(const char *header, char *out, size_t cap) {
  if (out == nullptr || cap < TOKEN_LEN + 1) {
    return false;
  }
  out[0] = '\0';
  if (header == nullptr) {
    return false;
  }
  static const char SCHEME[] = "bearer";
  size_t i = 0;
  for (; i < sizeof(SCHEME) - 1; i++) {
    char c = header[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != SCHEME[i]) {
      return false;
    }
  }
  if (header[i] != ' ') {
    return false;
  }
  while (header[i] == ' ') {
    i++;  // RFC 7235 allows more than one SP between the scheme and the credential
  }
  size_t n = 0;
  while (header[i + n] != '\0' && n <= TOKEN_LEN) {
    n++;
  }
  if (n != TOKEN_LEN) {
    return false;  // too short, or longer than a token can be (trailing junk included)
  }
  memcpy(out, header + i, TOKEN_LEN);
  out[TOKEN_LEN] = '\0';
  if (!validToken(out)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

}  // namespace AuthFmt
