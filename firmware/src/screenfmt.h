// usbdongle W3 — the parts of the status screen that are not pixels.
//
// Text fitting and dirty-region bookkeeping, split out of mod_display.cpp for
// one reason: they are the only parts of a display driver that can be tested
// without a display. `pio test -e native` excludes src/*.cpp
// (build_src_filter = -<*>), so anything that wants a host test has to live in
// a header — same arrangement as claims.h, ratelimit.h and pathsafe.h.
//
// Nothing in here knows about SPI, ST7735, colours or the Adafruit API.
// <stddef.h>/<stdint.h>/<stdio.h>/<string.h> only.
//
// ---- WHY FITTING IS ITS OWN FUNCTION ------------------------------------
//
// The panel is 160 px wide. At the 5x7 GFX font with its 1 px advance that is
// exactly 26 columns, and Adafruit_GFX does not clip text — it wraps to the
// next line (setTextWrap(true), the default) or draws off the edge into the
// next region (setTextWrap(false)). Either one corrupts a neighbouring region
// that the dirty tracker believes is still valid, so the corruption persists
// until something else happens to redraw that region. An SSID is
// user-controlled and up to 32 characters. So every string that reaches the
// panel goes through fit() or fitPrefixed() first, and the column budget is an
// argument rather than an assumption.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace ScreenFmt {

// Marks a string that did not fit. One column, unambiguous at 5x7, and not a
// character that appears in an SSID often enough to be confusing. '...' would
// cost three of the columns it is trying to save.
constexpr char TRUNC_MARK = '~';

// Copies `src` into `dst` occupying at most `cols` columns, truncating with
// TRUNC_MARK if it does not fit. Returns the number of columns written.
//
// Bounded by BOTH `cols` and `cap`: the caller states the column budget, the
// buffer states the byte budget, and neither is allowed to be violated by the
// other being wrong.
inline size_t fit(char *dst, size_t cap, const char *src, size_t cols) {
  if (dst == nullptr || cap == 0) {
    return 0;
  }
  size_t limit = cols;
  if (limit > cap - 1) {
    limit = cap - 1;
  }
  if (limit == 0) {
    dst[0] = '\0';
    return 0;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return 0;
  }

  size_t n = strlen(src);
  if (n <= limit) {
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
  }
  // Does not fit: keep limit-1 characters and mark the cut. At limit == 1 that
  // leaves the mark alone, which is still the truth ("there was more").
  memcpy(dst, src, limit - 1);
  dst[limit - 1] = TRUNC_MARK;
  dst[limit] = '\0';
  return limit;
}

// "AP " + a 32-character SSID on a 26-column panel. The prefix is what tells
// the operator what they are looking at, so it survives and the body is what
// gets cut — the opposite of what a single fit() over the joined string does.
// Returns the number of columns written.
inline size_t fitPrefixed(char *dst, size_t cap, const char *prefix, const char *body, size_t cols) {
  if (dst == nullptr || cap == 0) {
    return 0;
  }
  size_t limit = cols;
  if (limit > cap - 1) {
    limit = cap - 1;
  }
  if (limit == 0) {
    dst[0] = '\0';
    return 0;
  }

  size_t used = fit(dst, cap, prefix, limit);
  if (used >= limit) {
    return used;  // the prefix alone filled (or overfilled) the line
  }
  return used + fit(dst + used, cap - used, body, limit - used);
}

// "00:12:34", or "2d03:14" once it has been up long enough for hours to stop
// being the useful unit. millis() wraps at 49.7 days; that is the caller's
// problem to document, not this function's to hide.
inline size_t uptime(char *dst, size_t cap, uint32_t ms) {
  if (dst == nullptr || cap == 0) {
    return 0;
  }
  uint32_t secs = ms / 1000u;
  uint32_t days = secs / 86400u;
  uint32_t hours = (secs % 86400u) / 3600u;
  uint32_t mins = (secs % 3600u) / 60u;
  uint32_t rem = secs % 60u;
  int n;
  if (days > 0) {
    n = snprintf(dst, cap, "%ud%02u:%02u", (unsigned)days, (unsigned)hours, (unsigned)mins);
  } else {
    n = snprintf(dst, cap, "%02u:%02u:%02u", (unsigned)hours, (unsigned)mins, (unsigned)rem);
  }
  if (n < 0) {
    dst[0] = '\0';
    return 0;
  }
  return ((size_t)n < cap) ? (size_t)n : cap - 1;
}

// Heap figures in the four columns they deserve: "152k", "1.4M", "832".
// Deliberately not exact — the exact number is what the `status` action and
// the diag screen are for.
inline size_t compactBytes(char *dst, size_t cap, uint32_t bytes) {
  if (dst == nullptr || cap == 0) {
    return 0;
  }
  int n;
  if (bytes >= 1024u * 1024u) {
    uint32_t whole = bytes / (1024u * 1024u);
    // Remainder is < 1048576, so *10 peaks at 10,485,750 — comfortably inside
    // uint32_t. No rounding: 1.99 MB reads as "1.9M", never as "2.0M".
    uint32_t tenth = ((bytes % (1024u * 1024u)) * 10u) / (1024u * 1024u);
    n = snprintf(dst, cap, "%u.%uM", (unsigned)whole, (unsigned)tenth);
  } else if (bytes >= 1024u) {
    n = snprintf(dst, cap, "%uk", (unsigned)(bytes / 1024u));
  } else {
    n = snprintf(dst, cap, "%u", (unsigned)bytes);
  }
  if (n < 0) {
    dst[0] = '\0';
    return 0;
  }
  return ((size_t)n < cap) ? (size_t)n : cap - 1;
}

// ---- the HID badge state (backlog C3) -----------------------------------
//
// Three states, not two. The badge used to track LIVE state only
// (Registry::enabledAt), which left the most alarming case of all invisible:
// with `enable` sitting at AUTH_TOKEN, a network session CAN arm `hid` — it
// simply cannot reboot to bind it (`reboot` is AUTH_PHYSICAL, cmdauth.h). So
// the keyboard appears at the NEXT power-cycle, potentially with nothing
// connecting the two events. The screen is what connects them.
//
// The rule, and the reason it is a function rather than three lines inside a
// switch in mod_display.cpp: `pio test -e native` cannot compile that file, and
// the precedence below is a judgement, not an obvious truth.
//
//   live                -> HID_LIVE.  Keystrokes can be injected RIGHT NOW.
//   armed && !live      -> HID_ARMED. Binds at the next boot.
//   !armed && live      -> HID_LIVE.  Disarmed since boot, but the interface is
//                                     STILL BOUND, so the host still has a
//                                     keyboard. Live wins: the badge reports
//                                     the hazard, not the intent.
//   neither             -> HID_OFF.
enum HidBadge : uint8_t {
  HID_OFF = 0,
  HID_LIVE = 1,
  HID_ARMED = 2,
};

inline HidBadge hidBadge(bool armed, bool live) { return live ? HID_LIVE : (armed ? HID_ARMED : HID_OFF); }

}  // namespace ScreenFmt

// ===========================================================================
// Dirty-region tracking
// ===========================================================================
//
// THE PROBLEM IT SOLVES. A full 160x80 16 bpp repaint is 25,600 bytes over
// SPI. At the 40 MHz this panel is clocked at that is ~5.1 ms of transfer
// before any pixel generation, and it would happen on EVERY tick if the
// renderer did not know what changed — 25 Hz x 5.1 ms is a fifth of the
// cooperative scheduler's entire budget spent redrawing an unchanged screen,
// and it flickers while it does it.
//
// So the screen is a fixed set of horizontal regions, each with a dirty bit,
// and a tick draws at most N of them. takeNext() hands them out ROUND-ROBIN
// rather than lowest-index-first, which matters: with a per-tick cap, a region
// that goes dirty every frame (the activity animation) would otherwise starve
// every region below it forever.

namespace Dirty {

// One uint32_t of bits. Eight regions today; the static_assert in
// mod_display.cpp is what stops that growing past the word silently.
constexpr uint8_t MAX_REGIONS = 32;

struct Regions {
  uint32_t bits = 0;
  uint8_t count = 0;
  uint8_t cursor = 0;
};

inline void init(Regions &r, uint8_t count) {
  r.bits = 0;
  r.count = (count > MAX_REGIONS) ? MAX_REGIONS : count;
  r.cursor = 0;
}

inline void mark(Regions &r, uint8_t region) {
  if (region < r.count) {
    r.bits |= (uint32_t)1u << region;
  }
}

inline void markAll(Regions &r) {
  r.bits = (r.count >= MAX_REGIONS) ? 0xFFFFFFFFu : (((uint32_t)1u << r.count) - 1u);
}

inline void clearAll(Regions &r) { r.bits = 0; }

inline bool isDirty(const Regions &r, uint8_t region) {
  return region < r.count && (r.bits & ((uint32_t)1u << region)) != 0;
}

inline bool any(const Regions &r) { return r.bits != 0; }

inline uint8_t pending(const Regions &r) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < r.count; i++) {
    if (r.bits & ((uint32_t)1u << i)) {
      n++;
    }
  }
  return n;
}

// Next dirty region in round-robin order, clearing its bit. Returns r.count
// when nothing is dirty — the caller's loop bound, so "none" needs no separate
// sentinel value.
inline uint8_t takeNext(Regions &r) {
  if (r.count == 0 || r.bits == 0) {
    return r.count;
  }
  for (uint8_t n = 0; n < r.count; n++) {
    uint8_t i = (uint8_t)((r.cursor + n) % r.count);
    if (r.bits & ((uint32_t)1u << i)) {
      r.bits &= ~((uint32_t)1u << i);
      r.cursor = (uint8_t)((i + 1) % r.count);
      return i;
    }
  }
  return r.count;
}

}  // namespace Dirty
