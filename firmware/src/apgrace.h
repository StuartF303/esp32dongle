// usbdongle W2 — the grace window between "the phone left the AP" and "the
// session is over". Part of the HTTP transport's auth, added by the pairing
// change of 2026-08-24.
//
// WHY THIS EXISTS AT ALL. Stuart's pairing decision of 2026-08-24
// (ARCHITECTURE.md, "Pairing model") makes the AP single-client and gives the
// device exactly ONE session. That combination has a lockout in it, and this
// file is the fix:
//
//   one session slot + one AP slot + a PIN that only shows while unpaired
//     => a phone that silently drops the AP leaves the single session slot
//        held by a token its browser may never present again, while the LCD
//        shows nothing, because shouldShow() says the device is paired.
//
// The device would be stuck: paired to nobody, with no PIN on screen and no way
// to re-pair short of the USB cable. Phones do this constantly and without
// telling anyone — screen lock, walking out of range, iOS deciding a network
// with no internet is not worth holding.
//
// So association IS the session boundary, and it is only coherent as one
// BECAUSE the AP is single-client: with four client slots "a station left"
// would say nothing about whose session it was. Losing the station WHILE A
// SESSION IS LIVE starts a 90-second clock; if a station comes back inside it,
// the browser reconnects with the token already in its localStorage and the
// user is asked for nothing. If the clock runs out, the caller revokes the
// session and mints a new PIN, which puts pairing back on the LCD.
//
// With no session there is no clock at all — see the note on `sessionLive`
// below, which is where the reasoning for that lives.
//
// WHAT THIS COSTS, plainly: up to 90 seconds during which a phone that has
// genuinely gone away still holds the one session, so a second person cannot
// pair even though nobody is really connected. That is the direction the
// pairing model already chose (pairing.h says the same thing about the PIN),
// and 90 s was picked as long enough for a screen-lock/Wi-Fi-sleep bounce and
// short enough that a person who walks away does not lock the desk out.
//
// ---- DEPENDENCY-FREE ON PURPOSE -----------------------------------------
//
// Same rule as ratelimit.h / pairing.h / claims.h: <stdint.h> only, header-only
// and inline, so `pio test -e native` drives the whole state machine on this
// machine with a synthetic clock. The native env sets build_src_filter = -<*>,
// so logic that lives in mod_http.cpp cannot be host-tested at all — and the
// interesting cases here are a 90-second window and the 49.7-day millis() wrap,
// neither of which any hardware test would actually exercise.
//
// A COUNT, NOT AN IDENTITY — and that is a known limitation, written up as
// backlog C11 rather than fixed here. Any association cancels a departed
// holder's window, because this file cannot tell one station from another. The
// justification below leans on the AP being single-client, but the inference it
// really needs is "the only party who can associate is the session holder",
// which the PSK does not guarantee (`psk set` exists so the owner can share
// it). Read C11 before deciding this is a bug and fixing it in passing: binding
// the window to the holder means capturing the station MAC from
// esp_wifi_ap_get_sta_list(), which is a design decision about identifying
// clients by MAC and is stuart's to take.
//
// The station count is an INPUT, not something this file reads: mod_http.cpp
// polls WiFi.softAPgetStationNum() from its existing 250 ms tick and hands the
// number in. A Wi-Fi event handler would run on the Wi-Fi task and mutate
// session state from a third task, which this design deliberately avoids —
// polling on a tick that already runs costs nothing and keeps every mutation of
// the session table on the loop task.
//
// ---- millis() WRAPS ------------------------------------------------------
//
// Every comparison is `(int32_t)(now - deadline) >= 0`, never `now >= deadline`
// — the form ratelimit.h documents and the rest of this codebase uses. millis()
// wraps to 0 after 49.7 days; the naive form would treat a deadline just before
// the wrap as unreachable and hold the session open for the following 49.7
// days. The subtraction form is correct as long as the interval is under ~24.8
// days, which 90 s comfortably is.

#pragma once

#include <stdint.h>

namespace ApGrace {

// How long after the last station disassociates the session survives.
//
// Not tunable at runtime, deliberately: it is a security-relevant lifetime, and
// a knob for it would be a knob for "keep my session alive indefinitely".
constexpr uint32_t GRACE_MS = 90u * 1000u;

static_assert(GRACE_MS < 0x7FFFFFFFu, "GRACE_MS must stay inside the signed-difference window");

struct State {
  bool armed = false;       // the grace clock is running
  // EXPIRED has already been reported for this session and must not be
  // reported again. See the note on the once-only guarantee at update().
  bool fired = false;
  uint32_t deadlineMs = 0;  // millis() at which the session ends
};

enum Event : uint8_t {
  NONE = 0,     // nothing to do
  EXPIRED = 1,  // the window closed: revoke the session and mint a new PIN
};

// Feed it the current station count, whether a session is actually live, and
// the clock. Returns EXPIRED EXACTLY ONCE per window, so a caller that ticks
// every 250 ms does not revoke a session forty times.
//
// That guarantee comes from the `fired` LATCH, which is SET on expiry and
// stays set — it is cleared only when a station returns or the session ends,
// i.e. when the situation genuinely changes. It is not cleared "before
// returning", and the guarantee does not come from the caller doing anything
// with the event. The full reasoning, including the 39-expiries-an-hour bug
// that its absence produced, is at the `if (s.fired)` branch below.
//
// THE RULE, in one line: the clock runs while a live session has no station.
//
//   stations > 0            cancel. The reassociation case, and the common one:
//                           the phone is back and the token it holds is still
//                           the one in the session table, so there is nothing
//                           left to time.
//   no session              cancel. Nothing to end — see below, this is a
//                           decision and not an omission.
//   session, no station     arm at now + GRACE_MS if not already armed; expire
//                           once the deadline passes.
//
// ---- WHY `sessionLive` IS A PARAMETER (stuart, 2026-08-24) --------------
//
// The first cut of this armed on the 1 -> 0 station edge alone, and therefore
// also ran for a station that associated and left WITHOUT pairing — rotating
// the PIN 90 seconds later. That is now explicitly not wanted, and the
// reasoning is worth keeping because it is the kind of thing that gets
// re-litigated as an oversight:
//
//   * WHAT ROTATING WOULD BUY: nothing. The PIN is on the LCD in plain sight
//     the entire time the device is unpaired. Someone who associated and left
//     learned nothing they could not already read off the screen, so replacing
//     the digits defends against nobody.
//   * WHAT IT WOULD COST: the pairing flow itself. Someone walks up, joins the
//     AP, and starts reading the PIN — or scanning the pair QR, which carries
//     the PIN in the URL. Their phone bounces off a network with no internet,
//     as phones do. Ninety seconds later the digits change under them, mid-
//     typing, and the code they just scanned is dead. That is the exact flow
//     the QR work is built around, and it would look like a device fault.
//
// So: no session, no clock. A session is the only thing this window exists to
// protect, and its absence is not an event.
//
// The corollary, stated rather than discovered later: a live session with no
// station is enough to arm, without ever having SEEN the station. That is
// deliberate too — a session can only be created by a client that was
// associated at the time (POST /api/session arrives over the AP), so "session
// but no station" always means the station left, even if it left inside the
// same 250 ms tick that created the session and this file never observed it.
inline Event update(State &s, uint8_t stations, bool sessionLive, uint32_t nowMs) {
  if (stations > 0 || !sessionLive) {
    // Either the phone is back or the session is gone. Both are a genuinely new
    // situation, so the latch clears here and nowhere else.
    s.armed = false;
    s.fired = false;
    s.deadlineMs = 0;
    return NONE;
  }

  if (s.fired) {
    // ---- THE LATCH, and why it is not belt-and-braces -------------------
    //
    // Without it this function re-arms on the very next tick — the inputs are
    // unchanged, after all — and reports EXPIRED again 90 seconds later, and
    // every 90 seconds after that, rotating the PIN each time. In the real
    // caller that never happens because the tick revokes the session the
    // instant it sees EXPIRED, which makes sessionLive false above; a host
    // test driving update() alone caught it in an hour of simulated ticks and
    // saw 39 expiries instead of 1.
    //
    // Relying on the caller to make the guarantee true is exactly the kind of
    // coupling this header exists to avoid: the once-only contract is stated
    // here, so it is enforced here. An EXPIRED that a caller ignores stays
    // ignored rather than turning into a PIN that changes every 90 seconds.
    return NONE;
  }

  if (!s.armed) {
    s.armed = true;
    s.deadlineMs = nowMs + GRACE_MS;
    return NONE;
  }

  if ((int32_t)(nowMs - s.deadlineMs) >= 0) {
    s.armed = false;
    s.fired = true;
    s.deadlineMs = 0;
    return EXPIRED;
  }

  return NONE;
}

// Forget everything. Called when the module is disabled or the AP is down: a
// clock that keeps running with the radio off would revoke a session (and
// rotate a PIN) belonging to an AP that no longer exists.
inline void reset(State &s) { s = State{}; }

inline bool armed(const State &s) { return s.armed; }

// Milliseconds left in the window, 0 when no clock is running or it has already
// run out. For status(), so the web UI can render the "reconnecting" state with
// a real countdown rather than a spinner of unknown length.
inline uint32_t remainingMs(const State &s, uint32_t nowMs) {
  if (!s.armed) {
    return 0;
  }
  int32_t left = (int32_t)(s.deadlineMs - nowMs);
  return left > 0 ? (uint32_t)left : 0u;
}

}  // namespace ApGrace
