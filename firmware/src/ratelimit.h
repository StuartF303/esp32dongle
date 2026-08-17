// usbdongle W2 — PIN attempt rate limiter.
//
// DEPENDENCY-FREE ON PURPOSE (<stdint.h>/<stddef.h> only, header-only inline),
// so `pio test -e native` can drive the whole state machine on this machine
// with a synthetic clock. That matters more here than for most things: the
// interesting states are 15 minutes and 49 days apart, and neither is testable
// on hardware in any reasonable time.
//
// WHY. AuthFmt::PIN_LEN is 8 digits — 10^8 combinations. A fast loop over the
// AP's ~1-2 ms round trip finds that in a couple of days of grinding, and a
// 6-digit PIN in under an hour. The PIN is only meaningful WITH this file.
//
// The policy, chosen to punish a script without bricking the device for the
// person holding it:
//   * every consecutive failure sets a minimum wait before the next attempt,
//     doubling: 1s, 2s, 4s, 8s, 16s, then capped at 30s;
//   * MAX_FAILS consecutive failures lock the endpoint out entirely for
//     LOCKOUT_MS, after which the counter resets and the escalation starts over;
//   * any success resets everything.
//
// The lockout is deliberately NOT permanent and NOT persisted across a reboot.
// A permanent lockout is a denial of service that an attacker can trigger on
// purpose from the AP, and the only recovery would be USB — which is exactly
// the channel the user may not be near. Persisting it would also mean writing
// to NVS on every failed guess, i.e. handing an attacker a flash-wear weapon.
//
// ---- millis() WRAPS ------------------------------------------------------
//
// Every comparison here is `(int32_t)(now - deadline) >= 0`, never `now >=
// deadline`. millis() wraps to 0 after 49.7 days; the naive form then treats a
// deadline just before the wrap as unreachable and locks the endpoint out for
// the following 49.7 days. The subtraction form is correct across the wrap as
// long as the interval is under ~24.8 days, which every constant here is.

#pragma once

#include <stdint.h>

namespace RateLimit {

// Consecutive failures before the endpoint locks out.
constexpr uint8_t MAX_FAILS = 10;
// First penalty, doubled per subsequent failure.
constexpr uint32_t BASE_DELAY_MS = 1000;
// Ceiling on the doubling, so an attacker cannot push a legitimate user's next
// attempt hours out just by grinding first.
constexpr uint32_t MAX_DELAY_MS = 30000;
// Length of the full lockout after MAX_FAILS.
constexpr uint32_t LOCKOUT_MS = 15u * 60u * 1000u;

static_assert(LOCKOUT_MS < 0x7FFFFFFFu, "LOCKOUT_MS must stay inside the signed-difference window");

struct State {
  uint8_t fails = 0;             // consecutive failures since the last success/lockout expiry
  bool hasDeadline = false;      // a per-attempt penalty is in force
  uint32_t nextAttemptMs = 0;    // millis() at which the next attempt is allowed
  bool locked = false;           // MAX_FAILS reached; hard lockout
  uint32_t lockUntilMs = 0;      // millis() at which the lockout ends
  uint32_t totalFails = 0;       // lifetime counter, for status/telemetry only
  uint32_t lockouts = 0;         // lifetime lockout count, ditto
};

enum Decision : uint8_t {
  ALLOW = 0,   // try the PIN
  WAIT = 1,    // too soon after a failure; retryAfterMs says how long
  LOCKED = 2,  // locked out; retryAfterMs says how long
};

// Penalty after `fails` consecutive failures (1-based). Exposed so a test can
// assert the schedule rather than reimplement it.
inline uint32_t penaltyMs(uint8_t fails) {
  if (fails == 0) {
    return 0;
  }
  uint32_t d = BASE_DELAY_MS;
  for (uint8_t i = 1; i < fails; i++) {
    if (d >= MAX_DELAY_MS) {
      return MAX_DELAY_MS;
    }
    d *= 2u;
  }
  return d > MAX_DELAY_MS ? MAX_DELAY_MS : d;
}

// May this attempt proceed? MUTATES `s`: an expired lockout or an expired
// penalty is cleared here, which is what makes this the only place that has to
// know the clock. `retryAfterMs` is written on WAIT and LOCKED, zeroed on ALLOW.
inline Decision check(State &s, uint32_t nowMs, uint32_t *retryAfterMs) {
  uint32_t retry = 0;
  Decision d = ALLOW;

  if (s.locked) {
    if ((int32_t)(nowMs - s.lockUntilMs) >= 0) {
      // Lockout served. Start clean rather than resuming at MAX_FAILS-1: the
      // attacker has already paid 15 minutes, and resuming at the top of the
      // escalation would make a second lockout arrive after a single guess.
      s.locked = false;
      s.fails = 0;
      s.hasDeadline = false;
      s.nextAttemptMs = 0;
    } else {
      retry = s.lockUntilMs - nowMs;
      d = LOCKED;
    }
  }

  if (d == ALLOW && s.hasDeadline) {
    if ((int32_t)(nowMs - s.nextAttemptMs) >= 0) {
      s.hasDeadline = false;
    } else {
      retry = s.nextAttemptMs - nowMs;
      d = WAIT;
    }
  }

  if (retryAfterMs != nullptr) {
    *retryAfterMs = retry;
  }
  return d;
}

// Record a failed attempt. Call ONLY after check() returned ALLOW, or the
// escalation counts attempts that were never actually made.
inline void fail(State &s, uint32_t nowMs) {
  if (s.totalFails != 0xFFFFFFFFu) {
    s.totalFails++;
  }
  if (s.fails < 0xFF) {
    s.fails++;
  }
  if (s.fails >= MAX_FAILS) {
    s.locked = true;
    s.lockUntilMs = nowMs + LOCKOUT_MS;
    s.hasDeadline = false;
    if (s.lockouts != 0xFFFFFFFFu) {
      s.lockouts++;
    }
    return;
  }
  s.hasDeadline = true;
  s.nextAttemptMs = nowMs + penaltyMs(s.fails);
}

// Record a success: the escalation and any pending penalty are cleared. The
// lifetime counters are not, so status still reports what has been tried.
inline void success(State &s) {
  s.fails = 0;
  s.hasDeadline = false;
  s.nextAttemptMs = 0;
  s.locked = false;
  s.lockUntilMs = 0;
}

// Attempts left before a lockout. Reported to the caller so a mistyped PIN is
// distinguishable from being one guess from a 15-minute wait.
inline uint8_t remaining(const State &s) {
  if (s.locked || s.fails >= MAX_FAILS) {
    return 0;
  }
  return (uint8_t)(MAX_FAILS - s.fails);
}

}  // namespace RateLimit
