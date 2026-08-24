// usbdongle W3 — the pairing secrets' route to the out-of-band channel.
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
// ---- AND A QR PAYLOAD, AS OF 2026-08-24 (pass B1) -----------------------
//
// ARCHITECTURE.md §"QR pairing on the LCD": pairing is meant to be a camera
// scan rather than eight — now four — digits typed into a phone. That needs
// TWO codes, because no single QR can both join a network and open a page:
//
//   CODE_JOIN  a `WIFI:` payload. Shown while the AP is up and NO station is
//              associated: the user has not joined the network yet.
//   CODE_PAIR  the pair URL carrying the current PIN, `HTTP://192.168.4.1/4821`.
//              Shown once a station IS associated but no session exists.
//
// WHICH ONE IS THE PRODUCER'S DECISION, NOT THE RENDERER'S. mod_http.cpp owns
// the association count and owns both secrets; mod_display.cpp is a renderer
// that draws what it is handed. That division is the reason this can stay a
// one-way rendezvous — if the panel decided which code to show it would need
// the PSK and the station list, and both would then live in two modules.
//
// The three fields move TOGETHER, in one publish() call, and get() copies the
// whole record. A reader must never be able to observe CODE_PAIR next to a
// `WIFI:` payload, or a payload that carries a PIN the digits no longer match.
//
// ---- THE JOIN PAYLOAD IS A SECRET. ALL OF IT. --------------------------
//
// Read this before adding a field or a log line. Everything above is written
// about "the PIN" because until 2026-08-24 the PIN was the only secret that
// crossed this boundary. It no longer is:
//
//   * `WIFI:T:WPA;S:tdongle-a9d8;P:<passphrase>;;` CONTAINS THE AP PASSPHRASE
//     in clear. mod_http.cpp's SECRETS block treats psk_ as AUTH_PHYSICAL-only
//     — readable only by someone holding the USB cable — and a `WIFI:` payload
//     is that same value in a different wrapper.
//   * The PAIR payload contains the PIN, in a form a camera reads faster and
//     from further away than a person reads digits.
//
// So every discipline in this header applies to `payload` and `fallback`
// exactly as it applies to `pin`: not stored unless a consumer subscribed, no
// JSON representation, no transport, no event, no log; wiped by subscribe(false)
// and by withdraw(); and never reachable through `display.screen` over the wire.
// A caller that wants to add "just the SSID" or "just the version" to a status
// object should stop and check which of these two payloads it would be echoing.
//
// ---- WHY THE PIN TRUNCATES AND THE PAYLOAD DOES NOT --------------------
//
// publish() bounds an over-length PIN to MAX_PIN and stores the prefix. It
// REJECTS an over-length payload or fallback outright (it withdraws instead).
// The asymmetry is deliberate and is about what the failure looks like:
//
//   * A truncated PIN is visibly wrong — the digits on screen do not match
//     what the device will accept, the user gets `ok:false`, and they retry.
//   * A truncated `WIFI:` payload still ENCODES and still SCANS. It hands the
//     phone a valid-looking network with a silently wrong passphrase, or a
//     valid-looking URL with a silently wrong PIN. There is no safe prefix of
//     a QR payload, so there is no safe truncation of one.
//
// MAX_PAYLOAD is sized so that no legal input can reach that branch: the worst
// case is `WIFI:T:WPA;S:<ssid>;P:<psk>;;` with a 23-character SSID and the
// 63-character maximum WPA2 passphrase, both fully backslash-escaped —
// 13 + 46 + 3 + 126 + 2 = 190 bytes. The slack above that is for optional
// fields (`H:`, a different `T:`) a producer may add later.
//
// ---- HEADER-ONLY, ON PURPOSE --------------------------------------------
//
// Same reason as activity.h and claims.h: `pio test -e native` excludes
// src/*.cpp, so the policy predicate and the buffer handling are only
// host-testable if they live in a header. <stddef.h>/<stdint.h>/<string.h>
// only — no Arduino, no FreeRTOS. Note that the FIT decision (does this
// payload encode small enough to draw?) is NOT here: it needs the encoder, so
// it lives in qrfit.h, which is separately dependency-free in the same sense.
//
// ---- THREADING -----------------------------------------------------------
//
// publish()/withdraw() are called from mod_http.cpp's tick and teardown, both
// on the loop task. get() is called from the display's tick, also the loop
// task. Single-task by construction today; if a producer ever moves to the
// HTTP task this needs a lock, and the Snapshot copy in get() is what would
// tear — which is precisely why get() copies the whole record in one call
// rather than offering three accessors a reader could interleave.

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

// The QR payload. 190 is the worst legal `WIFI:` form (see the block comment);
// this is that, rounded up with room for optional fields. Over-length input is
// refused rather than truncated.
constexpr size_t MAX_PAYLOAD = 224;
static_assert(MAX_PAYLOAD >= 190, "MAX_PAYLOAD can no longer hold a fully-escaped 63-character PSK");

// The plain-text stand-in drawn INSTEAD of the QR when the payload will not
// fit on the panel at a scannable size (qrfit.h decides that, not this file).
// Sized for the WPA2 maximum passphrase, which is the case that forces it.
constexpr size_t MAX_FALLBACK = 64;
static_assert(MAX_FALLBACK >= 63 + 1, "the fallback must hold a 63-character WPA2 passphrase");

// WHICH CODE IS SHOWING. The producer decides; see the block comment.
enum Code : uint8_t {
  CODE_NONE = 0,  // nothing published
  CODE_JOIN = 1,  // a `WIFI:` payload — the phone has not joined the AP yet
  CODE_PAIR = 2,  // the pair URL with the PIN in its path — joined, not paired
};

// One record, copied in one call. See the THREADING note: three separate
// accessors would let a reader mix a code from before a rotation with a
// payload from after it.
struct Snapshot {
  Code code;
  char pin[MAX_PIN + 1];
  char payload[MAX_PAYLOAD + 1];
  char fallback[MAX_FALLBACK + 1];
};

// THE POLICY. Pure, so it is unit-tested rather than asserted in a comment.
constexpr bool shouldShow(bool apUp, uint32_t liveSessions) { return apUp && liveSessions == 0; }

namespace detail {

inline bool subscribed_ = false;
inline Code code_ = CODE_NONE;
inline char pin_[MAX_PIN + 1] = {0};
inline char payload_[MAX_PAYLOAD + 1] = {0};
inline char fallback_[MAX_FALLBACK + 1] = {0};
inline uint32_t seq_ = 0;

inline void wipe() {
  code_ = CODE_NONE;
  memset(pin_, 0, sizeof(pin_));
  memset(payload_, 0, sizeof(payload_));
  memset(fallback_, 0, sizeof(fallback_));
}

// Copy `src` into a fixed buffer, NUL-terminating. Only ever called after the
// length has been checked, so it cannot truncate.
inline void store(char *dst, size_t cap, const char *src, size_t n) {
  memset(dst, 0, cap);
  if (src != nullptr && n > 0) {
    memcpy(dst, src, n);
  }
}

inline size_t lenOf(const char *s) { return s == nullptr ? 0 : strlen(s); }

// True when `s` is byte-for-byte what is already stored. Used by publish() to
// stay idempotent against a 250 ms tick.
inline bool same(const char *stored, const char *s) { return strcmp(stored, s == nullptr ? "" : s) == 0; }

}  // namespace detail

// ---- consumer side ------------------------------------------------------

// `display` calls subscribe(true) in enable(), subscribe(false) in disable().
// Until it does, publish() stores NOTHING: with the LCD off neither the PIN
// nor the passphrase-bearing JOIN payload enters these buffers at all, so each
// exists in exactly one place (mod_http's pin_ / psk_, themselves zeroed when
// the AP goes down) instead of two.
inline void subscribe(bool on) {
  detail::subscribed_ = on;
  if (!on) {
    detail::wipe();
    detail::seq_++;
  }
}

inline bool subscribed() { return detail::subscribed_; }

// "Something is published". Keyed on the PIN, which every code carries: there
// is no state in which a payload is published without one.
inline bool visible() { return detail::pin_[0] != '\0'; }

// Which of the two codes is current, or CODE_NONE. Cheap enough to poll from a
// renderer's dirty check without copying 300 bytes of Snapshot every tick.
inline Code code() { return detail::code_; }

// Change counter, same role as Activity::seq(): the renderer polls it and only
// redraws when it moves.
inline uint32_t seq() { return detail::seq_; }

// Copies the whole record out. Returns false and zeroes `out` when nothing is
// published, so a caller has one branch rather than four.
//
// `out` holds two secrets when this returns true. A caller on the stack should
// memset it before returning, the way mod_display.cpp does.
inline bool get(Snapshot &out) {
  memset(&out, 0, sizeof(out));
  if (detail::pin_[0] == '\0') {
    out.code = CODE_NONE;
    return false;
  }
  out.code = detail::code_;
  memcpy(out.pin, detail::pin_, sizeof(out.pin));
  memcpy(out.payload, detail::payload_, sizeof(out.payload));
  memcpy(out.fallback, detail::fallback_, sizeof(out.fallback));
  return true;
}

// ---- producer side ------------------------------------------------------

// Publish one pairing record for out-of-band display. No-op unless a consumer
// subscribed.
//
// Withdraws (and returns) if ANY of these hold, so a caller does not need a
// second branch for the negative cases:
//   * code is CODE_NONE;
//   * `pin` is null or empty;
//   * `payload` or `fallback` is longer than its buffer — see "WHY THE PIN
//     TRUNCATES AND THE PAYLOAD DOES NOT" above. This is unreachable for legal
//     input and is a refusal, not a clamp.
//
// `payload` and `fallback` may be null or empty: that is a legitimate state
// (the PIN with no QR beside it), not an error.
//
// Idempotent: republishing the identical record does not move seq(), so
// calling this from a 250 ms tick does not cause a 4 Hz redraw.
inline void publish(Code code, const char *pin, const char *payload, const char *fallback) {
  if (!detail::subscribed_ || code == CODE_NONE || pin == nullptr || pin[0] == '\0') {
    if (detail::pin_[0] != '\0') {
      detail::wipe();
      detail::seq_++;
    }
    return;
  }

  size_t payloadLen = detail::lenOf(payload);
  size_t fallbackLen = detail::lenOf(fallback);
  if (payloadLen > MAX_PAYLOAD || fallbackLen > MAX_FALLBACK) {
    // A partial QR payload scans as a confidently wrong one. Refuse the whole
    // record rather than put a lie on the glass.
    if (detail::pin_[0] != '\0') {
      detail::wipe();
      detail::seq_++;
    }
    return;
  }

  // Compare the PIN against what would actually be STORED, not against the
  // argument: an over-length value truncates on the way in, so comparing the
  // full string would make every republication look like a change and repaint
  // the panel four times a second forever.
  size_t pinLen = strlen(pin);
  if (pinLen > MAX_PIN) {
    pinLen = MAX_PIN;
  }
  if (detail::code_ == code && strlen(detail::pin_) == pinLen && strncmp(detail::pin_, pin, pinLen) == 0 &&
      detail::same(detail::payload_, payload) && detail::same(detail::fallback_, fallback)) {
    return;  // unchanged
  }

  detail::wipe();
  detail::code_ = code;
  detail::store(detail::pin_, sizeof(detail::pin_), pin, pinLen);
  detail::store(detail::payload_, sizeof(detail::payload_), payload, payloadLen);
  detail::store(detail::fallback_, sizeof(detail::fallback_), fallback, fallbackLen);
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
