// usbdongle W0 — the OTA rollback-confirmation decision table.
//
// DEPENDENCY-FREE ON PURPOSE, exactly like claims.h / cmdauth.h / screenfmt.h /
// activity.h: <stddef.h> and <stdint.h> only, header-only and inline, so
// `pio test -e native` can compile and assert it on this machine. The native
// env sets build_src_filter = -<*>, so logic that lived only in otahealth.cpp
// would never be built for the host and would go untested.
//
// ---- WHY IT EXISTS (backlog S4, with its premise corrected) --------------
//
// S4 claimed CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE was not set. It IS set —
// ~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig
// lines 424 (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y) and 4392
// (CONFIG_APP_ROLLBACK_ENABLE=y) — and we flash the PREBUILT bootloader from
// that package, whose ELF carries the symbol `set_actual_ota_seq`, which IDF
// only compiles under that option.
//
// So the bootloader is ARMED and the app never confirmed itself. That is worse
// than "rollback does not work": an image delivered by OTA lands in
// ESP_OTA_IMG_PENDING_VERIFY, nothing calls
// esp_ota_mark_app_valid_cancel_rollback(), and at the NEXT restart the
// bootloader sees PENDING_VERIFY, rewrites it to ABORTED and boots the other
// slot. The first OTA would appear to work and then silently revert. We have
// not hit it only because a USB flash never produces PENDING_VERIFY: PlatformIO
// writes framework-arduinoespressif32/tools/partitions/boot_app0.bin at
// otadata's offset, and that file is NOT blank — sector 0 holds one valid entry
// (ota_seq = 1, ota_state = 0xFFFFFFFF i.e. UNDEFINED, crc 0x4743989a), sector
// 1 holds ota_seq = 0 which is invalid by definition. ota_seq 1 selects slot
// (1-1) % 2 = app0, and UNDEFINED is the state the bootloader boots "without
// limits". It is byte-for-byte the otadata in backup/factory_release/. That is
// why `info` reports ota_state UNDEFINED rather than nothing at all.
//
// ---- WHAT THIS FILE IS ---------------------------------------------------
//
// Just the state machine: (pending? x criteria x elapsed) -> confirm / roll
// back / keep waiting. No flash, no partitions, no millis(). otahealth.cpp
// supplies the inputs and performs whatever this decides.
//
// Testing it here rather than on the device is the whole point. The failure
// mode is silent AND delayed: a criteria mask assembled with the wrong operator
// produces a device that works perfectly today and quietly discards the next
// OTA at the following power cycle. Nothing about that shows up in a boot log.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace OtaDecide {

// ---- the health criteria -------------------------------------------------
//
// All five must hold before the running image marks itself valid. They are
// deliberately about THE PLATFORM being up, not about any particular feature
// working, because a confirmation gate that is stricter than "this build is
// usable" rolls back good builds.

constexpr uint8_t CRIT_TICKS = 1u << 0;      // the scheduler is dispatching us, minTicks times over
constexpr uint8_t CRIT_REGISTRY = 1u << 1;   // the module registry restored without a fatal error
constexpr uint8_t CRIT_ESSENTIAL = 1u << 2;  // the essential module (`cdc`) is enabled
constexpr uint8_t CRIT_CONSOLE = 1u << 3;    // the command core answered, or the grace period expired
constexpr uint8_t CRIT_UPTIME = 1u << 4;     // minimum uptime, so a crash loop cannot confirm itself
constexpr uint8_t CRIT_ALL = CRIT_TICKS | CRIT_REGISTRY | CRIT_ESSENTIAL | CRIT_CONSOLE | CRIT_UPTIME;
constexpr uint8_t CRIT_COUNT = 5;

// DELIBERATELY NOT A CRITERION: Wi-Fi / the `http` transport.
//
// `http` is opt-in and defaultEnabled=false (see its descriptor in
// mod_http.cpp and the module table in ARCHITECTURE.md section 4). Requiring
// the SoftAP to be up before confirming would roll back a perfectly good build
// on every device whose owner has the radio switched off — which is the
// factory default. The temptation is real, because "Wi-Fi and the HTTP server
// are up" is the example S4 itself suggested; it is wrong for this firmware,
// and would convert an optional feature into a boot requirement.
//
// Same reasoning excludes `storage` (no card is a legitimate state, and there
// is no card-detect pin — see CLAUDE.md), `display` (a dead panel is not a bad
// build) and `hid` (never default-enabled, by design).

// Stable, wire-visible names for the five bits. Used by the `ota` command's
// `criteria` object, so a caller can see WHICH one is outstanding rather than
// just a count.
inline const char *critName(uint8_t bit) {
  switch (bit) {
    case CRIT_TICKS:
      return "ticks";
    case CRIT_REGISTRY:
      return "registry";
    case CRIT_ESSENTIAL:
      return "essential";
    case CRIT_CONSOLE:
      return "console";
    case CRIT_UPTIME:
      return "uptime";
    default:
      return "?";
  }
}

// Iteration order for the five bits, so callers do not open-code a shift loop
// that silently stops matching CRIT_ALL when a sixth is added.
constexpr uint8_t CRIT_BITS[CRIT_COUNT] = {CRIT_TICKS, CRIT_REGISTRY, CRIT_ESSENTIAL, CRIT_CONSOLE, CRIT_UPTIME};

// ---- the timings ---------------------------------------------------------

struct Config {
  uint32_t tickMs;          // how often otahealth.cpp's scheduler task runs
  uint32_t minTicks;        // CRIT_TICKS threshold
  uint32_t minUptimeMs;     // CRIT_UPTIME threshold
  uint32_t consoleGraceMs;  // after this, CRIT_CONSOLE is met without anyone talking to us
  uint32_t windowMs;        // give up and roll back at this point
};

// The shipped numbers. All clocks are UPTIME (millis() since reset), not time
// since the health check started, so there is one clock and the `ota` command's
// "remaining" needs no explanation.
//
//   tickMs 250          — cheap; the task is one compare when not pending.
//   minTicks 40         — 10 s of the scheduler actually dispatching this task.
//                         Proves the cooperative loop is turning over rather
//                         than wedged in someone's blocking call.
//   minUptimeMs 30000   — stuart proposed 20-30 s; took the top of the range.
//                         This is the crash-loop guard: an image that panics
//                         and reboots inside 30 s can never reach its own
//                         mark-valid call, so the bootloader gets its rollback.
//   consoleGraceMs 20000— an OTA'd device on a desk with no cable attached must
//                         still be able to confirm. Below minUptimeMs on
//                         purpose: by the time uptime clears, the console
//                         criterion has already resolved one way or the other,
//                         so it never becomes the thing that stalls a good
//                         build.
//   windowMs 180000     — 3 minutes, deliberately generous (6x minUptimeMs).
//                         Nothing here is racing: the two criteria that can
//                         fail permanently (registry, essential) fail at boot
//                         and will not recover, and the window exists so the
//                         outcome is DECIDED AND ANNOUNCED rather than left in
//                         PENDING_VERIFY for the next unplug to resolve
//                         silently. See otahealth.cpp for why "sit there
//                         forever" is the one option that is not acceptable.
constexpr Config DEFAULTS = {250, 40, 30000, 20000, 180000};

// Hand-checked and then handed to the compiler, in the same spirit as
// mod_display.cpp's region tiling asserts.
static_assert(DEFAULTS.minTicks * DEFAULTS.tickMs <= DEFAULTS.minUptimeMs,
              "CRIT_TICKS cannot be reached before CRIT_UPTIME, so minTicks is doing nothing");
static_assert(DEFAULTS.consoleGraceMs <= DEFAULTS.minUptimeMs,
              "the console grace outlasts the uptime gate, so an unattended device stalls on CRIT_CONSOLE");
static_assert(DEFAULTS.minUptimeMs < DEFAULTS.windowMs, "the window closes before the earliest possible confirm");
static_assert(CRIT_BITS[CRIT_COUNT - 1] == CRIT_UPTIME, "CRIT_BITS is out of step with the bit list");

// ---- the inputs ----------------------------------------------------------

struct Inputs {
  uint32_t uptimeMs;      // millis()
  uint32_t ticks;         // completed runs of the health task
  bool registryFatal;     // ModuleRestoreReport::nvsTooLong — "NOTHING was restored"
  bool essentialEnabled;  // registry.isEnabled("cdc")
  bool consoleAnswered;   // Console::requestsAnswered() > 0, over ANY transport
};

inline uint8_t criteriaMet(const Inputs &in, const Config &cfg) {
  uint8_t met = 0;
  if (in.ticks >= cfg.minTicks) {
    met |= CRIT_TICKS;
  }
  if (!in.registryFatal) {
    met |= CRIT_REGISTRY;
  }
  if (in.essentialEnabled) {
    met |= CRIT_ESSENTIAL;
  }
  // "answered a line OR the grace expired with the app still alive". The OR is
  // the load-bearing part: a headless device nobody talks to is healthy.
  if (in.consoleAnswered || in.uptimeMs >= cfg.consoleGraceMs) {
    met |= CRIT_CONSOLE;
  }
  if (in.uptimeMs >= cfg.minUptimeMs) {
    met |= CRIT_UPTIME;
  }
  return met;
}

// ---- the decision --------------------------------------------------------

enum Verdict : uint8_t {
  IDLE = 0,      // not in PENDING_VERIFY — every USB flash. Do nothing, ever.
  WAIT = 1,      // pending, criteria not all met, window still open
  CONFIRM = 2,   // esp_ota_mark_app_valid_cancel_rollback()
  ROLLBACK = 3,  // esp_ota_mark_app_invalid_rollback_and_reboot()
};

inline const char *verdictName(Verdict v) {
  switch (v) {
    case IDLE:
      return "idle";
    case WAIT:
      return "wait";
    case CONFIRM:
      return "confirm";
    case ROLLBACK:
      return "rollback";
    default:
      return "?";
  }
}

// THE TABLE. Order matters: CONFIRM is tested before the window, so criteria
// that complete on the very tick the window closes still confirm. The opposite
// order would roll back a healthy image on a one-tick timing coincidence.
inline Verdict decide(bool pendingVerify, const Inputs &in, const Config &cfg) {
  if (!pendingVerify) {
    return IDLE;
  }
  if (criteriaMet(in, cfg) == CRIT_ALL) {
    return CONFIRM;
  }
  if (in.uptimeMs >= cfg.windowMs) {
    return ROLLBACK;
  }
  return WAIT;
}

// Milliseconds left before the window closes. Saturates at 0 rather than
// wrapping, which a plain (window - uptime) would do the moment uptime passes
// it — and this feeds a "seconds remaining" the operator reads.
inline uint32_t remainingMs(const Inputs &in, const Config &cfg) {
  return (in.uptimeMs >= cfg.windowMs) ? 0u : (cfg.windowMs - in.uptimeMs);
}

// Progress towards the EXPECTED confirm point (minUptimeMs), not towards the
// window. The normal case is that uptime is the last criterion to fall, so this
// reaches 100% just as the image confirms; against the 3-minute window it would
// crawl and look stuck. Clamped, because it is rendered.
inline uint8_t progressPct(const Inputs &in, const Config &cfg) {
  if (cfg.minUptimeMs == 0 || in.uptimeMs >= cfg.minUptimeMs) {
    return 100;
  }
  return (uint8_t)((uint64_t)in.uptimeMs * 100u / cfg.minUptimeMs);
}

}  // namespace OtaDecide
