// usbdongle W3 — the `WIFI:` join payload, and its escaping.
//
// ONE STRING, BUILT ONCE, TESTED ON THIS MACHINE:
//
//     WIFI:T:WPA;S:<ssid>;P:<passphrase>;;
//
// ARCHITECTURE.md §"QR pairing on the LCD" needs two codes, and this file owns
// the first of them — the one a phone camera turns into "join this network".
// The second (the pair URL, `HTTP://192.168.4.1/4821`) is a snprintf at its
// call site in mod_http.cpp and needs no file: it has no escaping problem,
// because an IPv4 address and four digits cannot contain a metacharacter.
// This one does, and that is the whole reason it exists separately.
//
// ---- THE FORMAT IS NOT AN RFC, AND ITS ESCAPING IS THE POINT -------------
//
// The `WIFI:` scheme is ZXing's MECARD-derived convention. Every Android and
// iOS camera implements it; nobody standardised it. Its field separator is
// `;`, its key/value separator is `:`, its list separator is `,`, `"` may
// quote a value, and `\` escapes any of them. So the FIVE characters
//
//     \   ;   ,   :   "
//
// must be backslash-escaped inside the S and P values, and nothing else may
// be. An unescaped `;` in a passphrase does not produce a broken QR — it
// produces a VALID one that a phone parses as the end of the P field, i.e. a
// silently truncated passphrase and an association that fails with no
// explanation on either side.
//
// ---- WHY THIS IS REACHABLE, WHICH IS THE ONLY REASON IT IS HERE ----------
//
// mod_http.cpp's generated passphrase (AuthFmt::makePsk, 4-4-5 groups of an
// unambiguous alphabet joined by '-') contains none of the five. If that were
// the only producer this file would be dead code dressed as diligence.
//
// It is not the only producer. `psk set` accepts ANY printable ASCII
// 0x20..0x7e, 8..63 characters — mod_http.cpp's passphraseRejection() spells
// that bound out — so `pass;word` is a supported input reachable by a
// documented command, and stuart has already exercised `psk set` once with a
// hand-chosen value. The escape is therefore on the live path for a real
// device, not a hypothetical one.
//
// ---- REFUSAL, NEVER TRUNCATION ------------------------------------------
//
// join() returns false and leaves `out` EMPTY when the payload would not fit.
// pairing.h states the general rule and the reasoning applies here verbatim:
// there is no safe prefix of a QR payload, because a truncated `WIFI:` string
// still encodes and still scans, handing the phone a confidently wrong
// passphrase. The caller's response to false is to publish no payload and let
// the panel print the passphrase as text (mod_display.cpp's REG_QR fallback),
// which is the same place a payload too big to DRAW ends up.
//
// ---- THE ARITHMETIC, SO THE BUFFER SIZES CAN BE CHECKED BY HAND ---------
//
//     "WIFI:T:WPA;S:"   13
//     ssid, escaped     <= 2 x 23   (mod_http.cpp's ssid_ is char[24])
//     ";P:"              3
//     psk, escaped      <= 2 x 63   (AuthFmt::PSK_MAX)
//     ";;"               2
//                       ------
//                        190  worst case, + 1 for the NUL
//
// which is exactly the number pairing.h sizes MAX_PAYLOAD (224) from.
//
// WHAT THE TEST ACTUALLY ASSERTS, since this comment previously claimed
// something it did not. It is NOT "the two files derive 224 independently and
// agree" — test_wifiqr had a third hardcoded 224 of its own, so lowering
// Pairing::MAX_PAYLOAD left the suite green and the claim was worth nothing.
// test_pairings_buffer_holds_the_worst_case_join_produces now RUNS this
// builder on the worst legal input and asserts Pairing::MAX_PAYLOAD holds the
// result. That is the property the buffer exists for; the specific number is
// not. If this comment and pairing.h's ever disagree, the test is what settles
// it — and now it can.
//
// The REAL cases are much smaller and are the rows of ARCHITECTURE.md's
// measured table:
//     tdongle-a9d8 + a generated 15-char PSK -> 45 chars -> QR version 3, drawable
//     tdongle-a9d8 + a 63-char owner-set PSK -> 93 chars -> QR version 5, NOT drawable
// The second is the row that forced qrfit.h's text fallback to exist, and
// test_wifiqr drives this builder's output straight into QrFit::encode() to
// prove both still land where the table says.
//
// ---- DEPENDENCY-FREE, SAME RULE AS ITS NEIGHBOURS -----------------------
//
// <stddef.h>/<stdint.h>/<string.h> and nothing else, header-only and inline,
// exactly as apgrace.h / pinpolicy.h / qrfit.h / pairing.h. `pio test -e
// native` sets build_src_filter = -<*>, so ANYTHING that lives in
// mod_http.cpp is untestable on this machine by construction — and this
// machine has no 802.11 PHY, so the handler that would exercise it over the
// air is unreachable here too. A header is the only place this can be covered.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace WifiQr {

// The literal parts. Named rather than inlined into a snprintf so the length
// arithmetic above can be asserted rather than trusted.
constexpr const char PREFIX[] = "WIFI:T:WPA;S:";
constexpr const char MID[] = ";P:";
constexpr const char SUFFIX[] = ";;";

// Bytes of fixed text in every payload: 13 + 3 + 2.
constexpr size_t FIXED_LEN = (sizeof(PREFIX) - 1) + (sizeof(MID) - 1) + (sizeof(SUFFIX) - 1);
static_assert(FIXED_LEN == 18, "the WIFI: envelope is no longer 18 bytes; the MAX_PAYLOAD arithmetic moves with it");

// The five metacharacters of the `WIFI:` grammar. See the header comment —
// this set is not "special characters in general", it is exactly what ZXing's
// parser treats as structure.
constexpr bool needsEscape(char c) { return c == '\\' || c == ';' || c == ',' || c == ':' || c == '"'; }

// Length `s` occupies inside an S or P field once escaped. A null string is
// zero rather than an error: the length of nothing is nothing, and join()
// decides separately whether nothing is acceptable there.
inline size_t escapedLen(const char *s) {
  if (s == nullptr) {
    return 0;
  }
  size_t n = 0;
  for (const char *p = s; *p != '\0'; p++) {
    n += needsEscape(*p) ? 2u : 1u;
  }
  return n;
}

// Bytes the finished payload will occupy, NOT counting the NUL. Exposed so a
// caller (and a test) can ask before allocating, and so the refusal in join()
// is a comparison against a published function rather than an internal one.
inline size_t joinLen(const char *ssid, const char *psk) {
  return FIXED_LEN + escapedLen(ssid) + escapedLen(psk);
}

namespace detail {

// Appends `s` escaped. The caller has already proved it fits; this cannot
// overflow and deliberately has no bound of its own, because a second,
// silently-clamping bound is how a "safe" copy becomes a truncated QR.
inline size_t appendEscaped(char *out, size_t at, const char *s) {
  for (const char *p = s; *p != '\0'; p++) {
    if (needsEscape(*p)) {
      out[at++] = '\\';
    }
    out[at++] = *p;
  }
  return at;
}

}  // namespace detail

// Builds the join payload into `out`.
//
// Returns false — with `out` set to the empty string whenever there is room
// for one byte — if:
//   * `out` is null or `cap` is 0;
//   * `ssid` is null or empty. A `WIFI:` payload with no S field names no
//     network and a phone shows it as a nameless entry it cannot join;
//   * `psk` is null or empty. THIS IS A REFUSAL, NOT A DEGRADATION TO AN OPEN
//     NETWORK. `T:WPA` with an empty `P:` tells the phone the network is WPA2
//     with a blank key, which is not a thing; the AP this device runs is
//     always WPA2-PSK (mod_http.cpp startAp) and a payload that says otherwise
//     would be a lie about the security of the link;
//   * the escaped result plus its NUL will not fit `cap`.
//
// The last case is the one that matters and is why this returns bool rather
// than a length: see "REFUSAL, NEVER TRUNCATION" above.
inline bool join(char *out, size_t cap, const char *ssid, const char *psk) {
  if (out == nullptr || cap == 0) {
    return false;
  }
  out[0] = '\0';
  if (ssid == nullptr || ssid[0] == '\0' || psk == nullptr || psk[0] == '\0') {
    return false;
  }
  size_t need = joinLen(ssid, psk);
  if (need + 1 > cap) {
    return false;
  }

  size_t at = 0;
  memcpy(out + at, PREFIX, sizeof(PREFIX) - 1);
  at += sizeof(PREFIX) - 1;
  at = detail::appendEscaped(out, at, ssid);
  memcpy(out + at, MID, sizeof(MID) - 1);
  at += sizeof(MID) - 1;
  at = detail::appendEscaped(out, at, psk);
  memcpy(out + at, SUFFIX, sizeof(SUFFIX) - 1);
  at += sizeof(SUFFIX) - 1;
  out[at] = '\0';
  return at == need;
}

}  // namespace WifiQr
