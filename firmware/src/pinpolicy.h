// usbdongle W2 — what a PIN attempt does to the rate limiter, and whether it
// mints a new PIN. Added by the pairing change of 2026-08-24.
//
// ---- WHY THIS IS A FILE AND NOT SIX LINES IN mod_http.cpp ---------------
//
// It was six lines in mod_http.cpp, interleaved with the constant-time compare
// and the session table, and that made the ONE INVARIANT THE 4-DIGIT PIN RESTS
// ON untestable on this machine:
//
//   THE LOCKOUT PATH MINTS A NEW PIN AND MUST NOT CLEAR THE LIMITER.
//
// authfmt.h explains why 10^4 is defensible at all: the limiter allows ~34
// guesses an hour, so a STATIC 4-digit PIN falls in about six days, and what
// removes that bound is minting a fresh PIN every time the limiter trips — each
// 17.5-minute cycle then spends its 10 guesses against a fresh space. If the
// mint also cleared the limiter, ten guesses would buy an instant reset, the
// 15-minute lockout would never be served, and the escalating delay would be
// the only control left. The limiter would be a no-op wearing the costume of
// one, and every comment in this repository about 10^4 being defensible would
// silently become false.
//
// test_ratelimit covers the limiter. test_ct covers the compare. The DECISION
// between them had zero coverage on either side, and it is not reachable from
// the host any other way: handleSessionCreate() is registered only on the
// softAP listener, and this machine has no 802.11 PHY to associate with. A host
// test of this header is the only coverage that can ever exist for it here.
//
// So: dependency-free, ratelimit.h and nothing else, header-only and inline,
// the same rule as claims.h / pairing.h / apgrace.h. No Arduino, no FreeRTOS,
// no ArduinoJson.
//
// ---- WHAT LIVES HERE AND WHAT DOES NOT ----------------------------------
//
// HERE: the limiter transition and the mint decision, which belong together
// because getting the pair wrong is the failure above.
//
// NOT HERE: the comparison itself (ct.h — this file takes its RESULT as a
// bool), the PIN generator (authfmt.h), where a PIN is stored, when the caller
// performs the mint, and anything that touches a session. This header knows
// nothing about sessions, sockets, NVS or the LCD.

#pragma once

#include <stdint.h>

#include "ratelimit.h"

namespace PinPolicy {

// What the caller must do about the PIN, and what was done to the limiter.
//
// `mint` is an INSTRUCTION: the caller must replace the current PIN.
// `clearLimiter` is a STATEMENT of what the call already did to the State it
// was handed — the transition is performed here, not delegated — exposed so a
// test can assert it and so a call site cannot quietly disagree with it.
struct Outcome {
  bool mint;
  bool clearLimiter;
};

// One PIN attempt. `correct` is the result of the constant-time comparison;
// this file never sees a PIN.
//
// PRECONDITION: RateLimit::check() returned ALLOW for this attempt. That is the
// limiter's own documented contract (an attempt that was refused must not be
// counted), and it is also what makes `s.locked` after fail() mean "this
// attempt tripped the lockout" rather than "a lockout was already in force".
//
// The three outcomes:
//   correct              -> {mint, clear}. The PIN is SINGLE-USE: pairing spends
//                           it, so it must be replaced. The limiter is cleared
//                           because whoever just proved they hold the current
//                           PIN is not the attacker it was escalating against.
//   failure 1..MAX-1     -> {no mint, no clear}. An ordinary wrong guess. The
//                           PIN stands; the escalating delay does its work.
//   failure MAX (locked) -> {MINT, NO CLEAR}. The invariant at the top of this
//                           file. A new PIN, and the 15 minutes still stand.
inline Outcome afterAttempt(RateLimit::State &s, bool correct, uint32_t nowMs) {
  if (correct) {
    RateLimit::success(s);
    return Outcome{true, true};
  }
  RateLimit::fail(s, nowMs);
  if (s.locked) {
    // NOT RateLimit::success(). Rotating the secret is not forgiveness for the
    // ten failures that forced the rotation. See the top of this file.
    return Outcome{true, false};
  }
  return Outcome{false, false};
}

// The operator regenerating the PIN over the USB cable (`http pin
// regenerate:true`, AUTH_PHYSICAL).
//
// THIS ONE DOES CLEAR THE LIMITER, and the difference from the lockout path is
// the whole reason both live in this file where they can be read together. The
// caller is holding the cable, so they are the owner of the device rather than
// the party the limiter defends against, and clearing a lockout for them is the
// documented way out of one — a limiter that cannot be cleared by the owner is
// a denial of service with no recovery. A network client can never reach this:
// AUTH_PHYSICAL means "is holding the cable" and no token can prove that.
inline Outcome operatorRegenerate(RateLimit::State &s) {
  RateLimit::success(s);
  return Outcome{true, true};
}

}  // namespace PinPolicy
