// usbdongle W2 — constant-time comparison for secrets that arrive over a network.
//
// DEPENDENCY-FREE ON PURPOSE, exactly like claims.h / pathsafe.h / b64.h:
// <stddef.h>, <stdint.h> and <string.h> only, header-only and inline, so
// `pio test -e native` can compile and assert it on this machine. The native
// env sets build_src_filter = -<*>, so a .cpp here would never be built for the
// host and this would go untested.
//
// WHY IT EXISTS. The HTTP transport compares two secrets against attacker-
// supplied input on every request: an 8-digit PIN and a 48-hex-character
// session token. The obvious `strcmp()` returns as soon as it finds a
// differing byte, so the time it takes leaks HOW MANY LEADING BYTES WERE
// RIGHT. Over a LAN that difference is small but measurable with enough
// samples, and it converts a 10^8 PIN search into ~10*8 guesses — i.e. it
// defeats the rate limiter as well, because each guess is a *legitimate*
// failed attempt that happens to be informative.
//
// The rule these functions obey: the number of loop iterations, and the set of
// bytes touched, depend ONLY on the declared buffer length — never on the
// contents of either operand and never on where the first difference is.
//
// What is deliberately NOT hidden: the LENGTH of the candidate. The attacker
// chose it, and the secret's length is a fixed, published format constant
// (AuthFmt::PIN_LEN / TOKEN_LEN), so there is nothing there to leak.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace CT {

// `n` bytes of `a` against `n` bytes of `b`, no early exit.
//
// `diff` is volatile so the compiler may not hoist the accumulation, spot that
// it can bail out once diff != 0, or replace the loop with memcmp. -O2 will
// happily do all three to a non-volatile version.
inline bool equal(const void *a, const void *b, size_t n) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  const unsigned char *pa = (const unsigned char *)a;
  const unsigned char *pb = (const unsigned char *)b;
  volatile unsigned int diff = 0;
  for (size_t i = 0; i < n; i++) {
    diff |= (unsigned int)(pa[i] ^ pb[i]);
  }
  return diff == 0;
}

// NUL-terminated `secret` against NUL-terminated `cand`, both bounded by
// `maxLen`. Always walks exactly `maxLen` positions whatever the two strings
// hold, so neither the position of the first difference nor the candidate's
// length changes the trip count.
//
// A length mismatch is folded into the same accumulator rather than returned
// early: an early `return false` on a short candidate is itself a timing
// signal, and a cheap one to remove.
inline bool equalStr(const char *secret, const char *cand, size_t maxLen) {
  if (secret == nullptr || cand == nullptr) {
    return false;
  }
  size_t ls = strnlen(secret, maxLen);
  size_t lc = strnlen(cand, maxLen);
  volatile unsigned int diff = (unsigned int)(ls ^ lc);
  for (size_t i = 0; i < maxLen; i++) {
    // Past the end of either string the byte is treated as 0. Reading the real
    // buffer past its NUL would be the alternative and is out of bounds.
    unsigned char x = (i < ls) ? (unsigned char)secret[i] : 0u;
    unsigned char y = (i < lc) ? (unsigned char)cand[i] : 0u;
    diff |= (unsigned int)(x ^ y);
  }
  return diff == 0;
}

}  // namespace CT
