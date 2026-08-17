// usbdongle W3 — "something is happening" reporting, decoupled from the LCD.
//
// WHY THIS EXISTS, AND WHY IT IS NOT IN mod_display.h: a long-running job
// (`storage.verify` today, an OTA write or an SD copy tomorrow) has to be able
// to say "I am doing X, I am N% through it" without knowing whether anything
// is looking. The moment a tool module includes a display header, the display
// stops being an optional peripheral: disabling it, or building an image
// without it, becomes a compile error in five other files.
//
// So the dependency runs one way only:
//
//     mod_storage.cpp ─┐
//     mod_http.cpp  ───┼──> activity.h <── mod_display.cpp
//     (future modules) ┘      (neutral)      (the only consumer today)
//
// Producers include this. The consumer includes this. Neither includes the
// other, and nothing here mentions a pixel.
//
// ---- NO-OP WHEN NOBODY IS LOOKING ---------------------------------------
//
// Every entry point returns immediately unless a consumer has called
// subscribe(true). `display` does that in its enable() and subscribe(false) in
// its disable(), so with the LCD off — or absent from the build — begin() /
// progress() / end() cost one load and one branch. That is deliberate: it must
// never be worth a module's while to guard its own reporting calls with an
// `if (displayEnabled)`, because that is exactly the coupling this file exists
// to prevent.
//
// The corollary, stated rather than hidden: enabling `display` half way
// through a job shows nothing until that job's next begin(). Nothing here
// replays history, and a job that is already 60% done is not worth a
// back-channel to discover.
//
// ---- HEADER-ONLY, ON PURPOSE --------------------------------------------
//
// Same reason as claims.h / ratelimit.h / pathsafe.h: `pio test -e native`
// excludes src/*.cpp entirely (build_src_filter = -<*>), so pure logic that
// wants host tests has to live in a header. This one includes <stdint.h>,
// <stddef.h> and <string.h> and nothing else — no Arduino.h, no FreeRTOS. Keep
// it that way.
//
// ---- THREADING -----------------------------------------------------------
//
// ONE PRODUCER AT A TIME. There is no lock: the state is a handful of scalars
// and two small buffers, and the only consumer is a redraw. If two tasks
// reported concurrently the worst case is one frame showing a mixed label —
// not a crash, not a leak. Adding a mutex would put a FreeRTOS dependency in a
// header that a host test compiles, to protect a progress bar. If a real
// second producer ever appears, that trade needs revisiting rather than
// assuming.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Activity {

// Bounded because everything here is bounded — the panel is 160 px wide and
// nothing may render a string it did not size. `who` is a module id
// (Registry::MAX_ID_LEN is 15, but the display has ~10 columns for it after
// the verb) and `verb` is one word: "verify", "copy", "erase", "flash".
constexpr size_t MAX_WHO = 12;
constexpr size_t MAX_VERB = 16;

enum State : uint8_t {
  IDLE = 0,       // nothing to show
  RUNNING = 1,    // begin() called, end() not yet
  DONE_OK = 2,    // end(true); the consumer decides how long to linger on it
  DONE_FAIL = 3,  // end(false)
};

struct Snapshot {
  State state;
  uint8_t pct;  // 0..100, always clamped
  uint32_t seq;
  char who[MAX_WHO + 1];
  char verb[MAX_VERB + 1];
};

namespace detail {

inline bool subscribed_ = false;
inline State state_ = IDLE;
inline uint8_t pct_ = 0;
inline uint32_t seq_ = 0;
inline char who_[MAX_WHO + 1] = {0};
inline char verb_[MAX_VERB + 1] = {0};

inline void copyBounded(char *dst, size_t cap, const char *src) {
  if (cap == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0;
  for (; i + 1 < cap && src[i] != '\0'; i++) {
    dst[i] = src[i];
  }
  dst[i] = '\0';
}

}  // namespace detail

// ---- consumer side ------------------------------------------------------

// Called by whatever renders this — `display`, and nothing else today.
// subscribe(false) also drops any in-flight state, so a job that was running
// when the LCD was turned off does not reappear when it is turned back on.
inline void subscribe(bool on) {
  detail::subscribed_ = on;
  if (!on) {
    detail::state_ = IDLE;
    detail::pct_ = 0;
    detail::who_[0] = '\0';
    detail::verb_[0] = '\0';
    detail::seq_++;
  }
}

inline bool subscribed() { return detail::subscribed_; }

// Monotonic change counter. The consumer polls this every tick — one load and
// one compare — and only rebuilds its strings when it moves. It is what makes
// "redraw only what changed" cheap enough to check at 25 Hz.
inline uint32_t seq() { return detail::seq_; }

// Copies the current state out. Returns false (and leaves `out` zeroed) when
// there is nothing to show.
inline bool snapshot(Snapshot &out) {
  out.state = detail::state_;
  out.pct = detail::pct_;
  out.seq = detail::seq_;
  detail::copyBounded(out.who, sizeof(out.who), detail::who_);
  detail::copyBounded(out.verb, sizeof(out.verb), detail::verb_);
  return out.state != IDLE;
}

// Consumer-side: drop a finished job back to IDLE, after it has been on screen
// long enough to read. Producers never call this — end() is theirs, clear() is
// the renderer's, and keeping them separate is what lets a 200 ms job still be
// visible for two seconds.
inline void clear() {
  if (detail::state_ == RUNNING || detail::state_ == IDLE) {
    return;
  }
  detail::state_ = IDLE;
  detail::pct_ = 0;
  detail::who_[0] = '\0';
  detail::verb_[0] = '\0';
  detail::seq_++;
}

// ---- producer side ------------------------------------------------------

// Start reporting. `who` is the module id, `verb` what it is doing. Both are
// truncated, never rejected: a reporting call must not be able to fail.
// A second begin() while one is running simply replaces it — the alternative
// (refuse, and leave the old label up) shows the wrong thing for longer.
inline void begin(const char *who, const char *verb) {
  if (!detail::subscribed_) {
    return;
  }
  detail::copyBounded(detail::who_, sizeof(detail::who_), who);
  detail::copyBounded(detail::verb_, sizeof(detail::verb_), verb);
  detail::state_ = RUNNING;
  detail::pct_ = 0;
  detail::seq_++;
}

// Safe to call on every tick of the reporting job: the sequence counter only
// moves when the percentage actually changes, so a caller does not have to
// rate-limit itself and the consumer does not redraw for nothing.
inline void progress(uint8_t pct) {
  if (!detail::subscribed_ || detail::state_ != RUNNING) {
    return;
  }
  if (pct > 100) {
    pct = 100;
  }
  if (pct == detail::pct_) {
    return;
  }
  detail::pct_ = pct;
  detail::seq_++;
}

// done/total convenience. 64-bit intermediate: 4 GB * 100 overflows uint32_t
// at a shade over 42 MB, and SD files are bigger than that.
inline void progressBytes(uint32_t done, uint32_t total) {
  if (total == 0) {
    progress(100);
    return;
  }
  if (done > total) {
    done = total;
  }
  progress((uint8_t)((uint64_t)done * 100u / total));
}

// Finish. `ok` is rendered — a failed verify must not look like a successful
// one just because both stop moving. Calling end() without a begin() is a
// no-op rather than a state, so a cancel path that runs twice is harmless.
inline void end(bool ok) {
  if (!detail::subscribed_ || detail::state_ != RUNNING) {
    return;
  }
  detail::state_ = ok ? DONE_OK : DONE_FAIL;
  if (ok) {
    detail::pct_ = 100;
  }
  detail::seq_++;
}

}  // namespace Activity
