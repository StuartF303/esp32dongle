// usbdongle W3 — the pairing PIN's route to the out-of-band channel.
//
// ARCHITECTURE.md section 4: "a per-device PIN shown on the LCD, exchanged for
// a session token ... The LCD is a real security asset here — it gives us an
// out-of-band channel most IoT devices lack." This header is that route, and
// it is deliberately the ONLY one.
//
// ---- WHY NOT JUST PUT IT IN http's status() -----------------------------
//
// Because status() is wire-visible. Registry::list() renders every enabled
// module's status into `GET /api/modules` and into the `modules` command on
// the WebSocket and the CDC console. mod_http.cpp says it plainly (the SECRETS
// block at the top): the PSK and PIN "are never logged, never emitted as an
// event, and never rendered into status()". A `pin` field there would hand the
// pairing secret to every already-authenticated client, forever, over the
// network — which is the exact opposite of what an out-of-band channel is for.
//
// So the PIN goes to the display through a private in-RAM rendezvous that has
// no JSON representation, no action, and no transport. `http` publishes,
// `display` reads, and no other file includes this header.
//
// ---- THE POLICY, STATED SO IT CAN BE RECONSIDERED -----------------------
//
// shouldShow() is the whole policy and it is one line: the PIN is on screen
// while the AP is up AND no session has been established. Stuart's decision,
// 2026-08-17. The reasoning:
//
//   * It only needs to be readable while someone is trying to pair.
//   * A PIN left on screen permanently is a PIN that anyone who walks past the
//     device has, and this device is meant to sit in the front of a machine in
//     an office.
//   * Once a session exists, whoever needed it has it.
//
// The cost, equally plainly: a second person cannot pair while the first is
// connected without either waiting for the session to expire, revoking it, or
// reading the PIN over USB. That is the intended direction — pairing is not
// meant to be a thing bystanders can do at will.
//
// It comes BACK when the last session expires or is revoked, because at that
// point the device is unpaired again and pairing must be possible.
//
// ---- WHAT PUBLISH() NOW CARRIES (changed 2026-08-24) --------------------
//
// The predicate is unchanged, but the VALUE is no longer stable. The PIN is
// 4 digits, lives in RAM only, is never written to NVS, and mod_http.cpp mints
// a fresh one on power-up/AP enable, ON USE (it is single-use — the moment it
// is exchanged for a token it is spent), on session end (an explicit unpair, or
// 90 s after the single AP client disassociates), and on every rate-limiter
// lockout. So publish() may
// arrive with a different string at any time and the panel must simply follow
// it — seq() already makes that a redraw. Nothing downstream may cache the PIN,
// print it into a scrollback, or treat "the PIN" as a fact about the device
// that outlives the current pairing attempt: it is a one-shot token, and
// shouldShow() now describes its whole lifetime rather than just its
// visibility.
//
// ---- HEADER-ONLY, ON PURPOSE --------------------------------------------
//
// Same reason as activity.h and claims.h: `pio test -e native` excludes
// src/*.cpp, so the policy predicate and the buffer handling are only
// host-testable if they live in a header. <stddef.h>/<stdint.h>/<string.h>
// only — no Arduino, no FreeRTOS.
//
// ---- THREADING -----------------------------------------------------------
//
// publish()/withdraw() are called from mod_http.cpp's tick and teardown, both
// on the loop task. get() is called from the display's tick, also the loop
// task. Single-task by construction today; if a producer ever moves to the
// HTTP task this needs a lock, and the buffer copy is what would tear.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Pairing {

// AuthFmt::PIN_LEN is 4 today (it was 8 until 2026-08-24). Left at 16 on
// purpose, and NOT tracked down to 4: this buffer exists so that changing the
// PIN format does not silently truncate the thing on screen, which is exactly
// the failure a tightly-sized buffer would produce the next time the length
// moves. publish() bounds to MAX_PIN anyway, so the slack costs 12 bytes of
// .bss and buys immunity to a format change.
constexpr size_t MAX_PIN = 16;

// THE POLICY. Pure, so it is unit-tested rather than asserted in a comment.
constexpr bool shouldShow(bool apUp, uint32_t liveSessions) { return apUp && liveSessions == 0; }

namespace detail {

inline bool subscribed_ = false;
inline char pin_[MAX_PIN + 1] = {0};
inline uint32_t seq_ = 0;

inline void wipe() {
  memset(pin_, 0, sizeof(pin_));
}

}  // namespace detail

// ---- consumer side ------------------------------------------------------

// `display` calls subscribe(true) in enable(), subscribe(false) in disable().
// Until it does, publish() stores NOTHING: with the LCD off the PIN never
// enters this buffer at all, so it exists in exactly one place (mod_http's
// pin_, itself zeroed when the AP goes down) instead of two.
inline void subscribe(bool on) {
  detail::subscribed_ = on;
  if (!on) {
    detail::wipe();
    detail::seq_++;
  }
}

inline bool subscribed() { return detail::subscribed_; }

inline bool visible() { return detail::pin_[0] != '\0'; }

// Change counter, same role as Activity::seq(): the renderer polls it and only
// redraws when it moves.
inline uint32_t seq() { return detail::seq_; }

// Copies the PIN out; returns its length, or 0 if nothing is published.
inline size_t get(char *out, size_t cap) {
  if (out == nullptr || cap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (detail::pin_[0] == '\0') {
    return 0;
  }
  size_t i = 0;
  for (; i + 1 < cap && detail::pin_[i] != '\0'; i++) {
    out[i] = detail::pin_[i];
  }
  out[i] = '\0';
  return i;
}

// ---- producer side ------------------------------------------------------

// Publish the PIN for out-of-band display. No-op unless a consumer subscribed.
// A null or empty `pin` withdraws, so a caller does not need a second branch.
// Idempotent: republishing the same value does not move seq(), so calling this
// from a 250 ms tick does not cause a 4 Hz redraw.
inline void publish(const char *pin) {
  if (!detail::subscribed_ || pin == nullptr || pin[0] == '\0') {
    if (detail::pin_[0] != '\0') {
      detail::wipe();
      detail::seq_++;
    }
    return;
  }
  // Compare against what would actually be STORED, not against the argument:
  // an over-length value truncates on the way in, so comparing the full string
  // would make every republication look like a change and repaint the panel
  // four times a second forever.
  size_t n = strlen(pin);
  if (n > MAX_PIN) {
    n = MAX_PIN;
  }
  if (strlen(detail::pin_) == n && strncmp(detail::pin_, pin, n) == 0) {
    return;  // unchanged
  }
  detail::wipe();
  memcpy(detail::pin_, pin, n);
  detail::pin_[n] = '\0';
  detail::seq_++;
}

// Take it off the screen and out of RAM. Called when the policy says so, and
// unconditionally when the AP comes down.
inline void withdraw() {
  if (detail::pin_[0] == '\0') {
    return;
  }
  detail::wipe();
  detail::seq_++;
}

}  // namespace Pairing
