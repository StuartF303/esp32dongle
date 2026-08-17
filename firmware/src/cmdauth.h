// usbdongle W2 — the authorisation policy for BUILT-IN commands.
//
// DEPENDENCY-FREE ON PURPOSE, exactly like claims.h / ct.h / pathsafe.h /
// screenfmt.h: <stddef.h>, <stdint.h>, <stdio.h> and <string.h> only,
// header-only and inline, so `pio test -e native` can compile and assert it on
// this machine. The native env sets build_src_filter = -<*>, so a policy that
// lived only in console.cpp would never be built for the host and would go
// untested.
//
// ---- WHY IT EXISTS (backlog S1) -----------------------------------------
//
// Modules gate themselves (mod_hid.cpp's requireInjectAuth, mod_storage.cpp's
// requireAuth). The BUILT-INS did not: `info`, `parts`, `mem`, `uptime`,
// `tasks`, `bootprobe`, `selftest`, `help`, `log`, `modules`, `enable`,
// `disable` and `reboot` were dispatched from console.cpp's table without ever
// reading ctx.authLevel. Nothing was exposed, because mod_http.cpp returns 401
// before it ever calls Console::execute() — which made ONE transport the entire
// security boundary in a design whose whole premise is that the command layer
// decides for itself, so three transport adapters cannot disagree. BLE is the
// next transport and would have inherited exactly that trap.
//
// So the level lives HERE, in one table, and console.cpp enforces it in one
// place before any handler runs. The property this buys, and the one the BLE
// adapter is meant to rely on: A TRANSPORT MAY DISPATCH AT AUTH_NONE SAFELY.
//
// ---- THE LEVELS, decided by stuart 2026-08-17 ---------------------------
//
//   reboot              AUTH_PHYSICAL  — only someone holding the cable may
//                                        restart the device. (Also closes S2.)
//   every other built-in AUTH_TOKEN
//   nothing             AUTH_NONE
//
// ONE EXCEPTION, added with the OTA rollback work (S4): `ota` is a TOKEN row
// whose three mutating parameters (confirm, rollback, boot) sit at
// OTA_MUTATE == PHYSICAL. See the block above that constant for why, and for
// why it is the only per-parameter gate among the built-ins.
//
// Note what is deliberately NOT here: `enable`/`disable` are AUTH_TOKEN even
// for a bootTimeBinding module like `hid`, i.e. a network session may ARM the
// keyboard. Stuart's call, on the grounds that arming is inert until a reboot
// it cannot perform. The agreed mitigation is visibility, not a higher level —
// the LCD shows "armed, binds at next boot" as its own badge (mod_display.cpp).

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace CmdAuth {

// A MIRROR of AuthLevel (registry.h), which cannot be included here: it drags
// in ArduinoJson and FreeRTOS and would take this header off the host. The
// three static_asserts in console.cpp are what keep the two definitions
// identical; there is no other link between them.
constexpr uint8_t NONE = 0;
constexpr uint8_t TOKEN = 1;
constexpr uint8_t PHYSICAL = 2;

inline const char *levelName(uint8_t level) {
  switch (level) {
    case NONE:
      return "none";
    case TOKEN:
      return "token";
    case PHYSICAL:
      return "physical";
    default:
      return "?";
  }
}

// The whole gate. Levels are ordered and cumulative — AUTH_PHYSICAL implies
// AUTH_TOKEN — which is why this is >= and not ==.
// constexpr, so the module-side policy (modauth.h) can express its own
// decisions in terms of THIS function rather than restating `>=`. Still inline
// and still callable at runtime; nothing about the built-in gate changed.
constexpr bool permits(uint8_t have, uint8_t need) { return have >= need; }

// ---- the policy table ---------------------------------------------------

struct Policy {
  const char *name;  // the built-in's `act`, exactly as console.cpp spells it
  uint8_t minAuth;
};

// SOURCE OF TRUTH for the levels. console.cpp's COMMANDS table initialises its
// `minAuth` field from requiredFor(), and a static_assert there fails the build
// if a command is missing from here or a name here matches no command — so
// these cannot drift apart silently.
constexpr Policy BUILTINS[] = {
    {"help", TOKEN},      {"info", TOKEN},     {"parts", TOKEN},     {"mem", TOKEN},
    {"uptime", TOKEN},    {"tasks", TOKEN},    {"led", TOKEN},       {"modules", TOKEN},
    {"enable", TOKEN},    {"disable", TOKEN},  {"selftest", TOKEN},  {"log", TOKEN},
    {"bootprobe", TOKEN},
    // READ-ONLY at TOKEN; its two MUTATING parameters are gated separately at
    // OTA_MUTATE below. Reading is deliberately available to a phone session:
    // once S5 lands, the thing that pushed an update is the thing that needs to
    // see whether it was confirmed, and a device that cannot report its own OTA
    // state to the client that updated it is worse than useless.
    {"ota", TOKEN},
    // The only one above TOKEN. A phone with a valid session can do a great
    // deal to this device, but it cannot take it away from the person at the
    // desk.
    {"reboot", PHYSICAL},
};
constexpr size_t BUILTIN_COUNT = sizeof(BUILTINS) / sizeof(BUILTINS[0]);

// ---- parameter-level elevation ------------------------------------------
//
// A COMMAND's row above is a FLOOR, not the whole story. `ota` reads at TOKEN
// but its `confirm`, `rollback` and `boot` parameters are terminal decisions
// about which image this device boots — `rollback` restarts the chip into the
// other slot, and `boot` picks the slot the bootloader starts next — so they
// sit at PHYSICAL alongside `reboot`. Three parameters, ONE gate: the check in
// cmdOta() covers whichever was asked for, so a fourth cannot be added without
// passing through it.
//
// This is the FIRST built-in to raise its own bar per-parameter, and it is
// worth being blunt about the trade. S1's rule is "no built-in checks auth for
// itself", enforced centrally in Console::execute(); an extra check inside a
// handler is a second gate that a reader of BUILTINS alone would not see. The
// alternative — putting the whole command at PHYSICAL — is consistent but makes
// the OTA state invisible to the only client that will ever perform an OTA.
//
// The mitigations: the elevated level is written HERE, next to the table, not
// as a literal in console.cpp; it is only ever HIGHER than the row (a
// parameter can never open a door the row closed, because execute()'s gate has
// already run by then); and it goes through the same CmdAuth::denyMessage(), so
// the refusal is byte-for-byte a module's refusal.
//
// Modules already do exactly this (mod_display.cpp's backlight, mod_hid.cpp's
// requireInjectAuth), so the pattern is not new to the image — only to the
// built-ins.
// (The assert that this only ever RAISES the level is below requiredFor(),
// which cannot be called before it is declared.)
constexpr uint8_t OTA_MUTATE = PHYSICAL;

// What an UNLISTED command requires. PHYSICAL, i.e. fail CLOSED: a built-in
// added to console.cpp without a policy entry is refused to every network
// caller rather than opened to them. The static_assert in console.cpp turns
// that into a build failure anyway; this is the behaviour if the assert is ever
// weakened.
constexpr uint8_t UNLISTED = PHYSICAL;

// constexpr strcmp-for-equality. Recursive rather than looped so it is a
// constant expression under every -std this project builds with.
constexpr bool streq(const char *a, const char *b) {
  return (a == nullptr || b == nullptr) ? (a == b) : ((*a != *b) ? false : (*a == '\0' ? true : streq(a + 1, b + 1)));
}

constexpr uint8_t requiredFor(const char *name) {
  for (size_t i = 0; i < BUILTIN_COUNT; i++) {
    if (streq(BUILTINS[i].name, name)) {
      return BUILTINS[i].minAuth;
    }
  }
  return UNLISTED;
}

constexpr bool isListed(const char *name) {
  for (size_t i = 0; i < BUILTIN_COUNT; i++) {
    if (streq(BUILTINS[i].name, name)) {
      return true;
    }
  }
  return false;
}

// The invariant for OTA_MUTATE, declared above: a parameter gate may only RAISE
// its command's level. Execute()'s central gate has already run by the time a
// handler sees the request, so a lower value here would not open anything — it
// would simply be dead, misleading code claiming a gate that does nothing.
static_assert(OTA_MUTATE >= requiredFor("ota"), "a parameter gate may only raise its command's level, never lower it");
static_assert(isListed("ota"), "the `ota` built-in has no policy row");

// The LOWEST level any built-in accepts — TOKEN today.
//
// Used for the pre-flight check in Console::execute(): a caller below this
// cannot run ANY built-in, so it is refused before the command name is even
// looked up. Without that, an AUTH_NONE client could still tell EUNKNOWN from
// EAUTH and enumerate the entire built-in surface of a device it has no session
// on. Derived, not written down twice: adding an AUTH_NONE command to the table
// is all it takes to open the door, deliberately.
constexpr uint8_t minimumLevel() {
  uint8_t lowest = PHYSICAL;
  for (size_t i = 0; i < BUILTIN_COUNT; i++) {
    if (BUILTINS[i].minAuth < lowest) {
      lowest = BUILTINS[i].minAuth;
    }
  }
  return lowest;
}

// ---- the "as" downgrade (design-capture aid, added 2026-08-17) ----------
//
// Console::execute() accepts an optional request-envelope field
// {"as":"token"} that asks to be DISPATCHED AND RENDERED as though the
// caller held a lower AuthLevel than it actually does. It exists because the
// only privileged transport on this dev machine is the USB cable
// (AUTH_PHYSICAL), so every real capture from here answers `allowed: true`
// to everything — there is no way, from this machine, to produce a genuine
// sample of what a lower-level client actually receives. `as` lets the cable
// ask for that sample honestly, by actually going through the same gate a
// lower-level caller would hit, rather than by hand-editing a capture that is
// documented as real device output.
//
// STRICTLY DOWNGRADE-ONLY. Why that is safe: lowering your own privilege can
// only ever REDUCE what a caller can do, so `as` cannot grant anything — the
// gate downstream still runs against the (now lower) level, so
// `{"mod":"storage","act":"format","as":"token"}` is refused by that gate
// exactly as a real token-level caller's `format` would be, and the format
// code is never reached. The only real risk is a reader mistaking `as` for a
// way to GRANT a level, which is why a request for MORE than the caller's own
// level is not silently clamped down to it — that would look, to a skim
// reader of a captured sample, indistinguishable from a caller that simply
// asked for less. It is refused outright as EARGS in Console::execute().
//
// The escalation case is made impossible BY CONSTRUCTION, not merely tested
// for: `effective` below is computed with a plain min(), so for every input —
// including a value a future bug might fail to reject as ESCALATION — the
// returned level can mathematically never exceed `actual`. The ESCALATION
// outcome exists to give the caller a clear, explicit error, not to make the
// downgrade itself safe; safety is the min().
enum class AsOutcome : uint8_t {
  NO_REQUEST,   // no "as" field present; `effective` == `actual`, unchanged
  OK,           // parsed and <= actual; `effective` is the (possibly lower) level to use
  ESCALATION,   // parsed but > actual — a clear error, never a clamp
  UNPARSEABLE,  // present but not one of "none"/"token"/"physical"
};

struct AsResolution {
  AsOutcome outcome;
  // Valid for NO_REQUEST and OK. For ESCALATION/UNPARSEABLE this is left at
  // `actual` (never higher, never garbage) so that even a caller which
  // forgets to check `outcome` cannot end up using a level above its own —
  // belt-and-braces on top of the min() above, not a substitute for checking
  // `outcome`.
  uint8_t effective;
};

// Parses one of the existing level names ("none"/"token"/"physical") — the
// SAME spelling levelName() produces, so a captured `min_auth`/`auth` string
// can be fed straight back in as an `as` value. Case-sensitive, exact match
// only (matching isListed()'s lookup style elsewhere in this file).
inline bool parseLevel(const char *name, uint8_t *out) {
  if (name == nullptr || out == nullptr) {
    return false;
  }
  if (streq(name, "none")) {
    *out = NONE;
    return true;
  }
  if (streq(name, "token")) {
    *out = TOKEN;
    return true;
  }
  if (streq(name, "physical")) {
    *out = PHYSICAL;
    return true;
  }
  return false;
}

// The pure decision behind the "as" envelope field. `requested` is the raw
// "as" string from the request (nullptr if the field was absent); `actual` is
// the caller's real AuthLevel. See the block comment above for what each
// outcome means and why `effective` can never exceed `actual`.
inline AsResolution resolveAs(const char *requested, uint8_t actual) {
  if (requested == nullptr) {
    return {AsOutcome::NO_REQUEST, actual};
  }
  uint8_t wanted;
  if (!parseLevel(requested, &wanted)) {
    return {AsOutcome::UNPARSEABLE, actual};
  }
  // min(), not an if-clamp: this line is what makes ESCALATION structurally
  // incapable of granting anything, independent of the outcome check below.
  uint8_t effective = (wanted < actual) ? wanted : actual;
  AsOutcome outcome = (wanted > actual) ? AsOutcome::ESCALATION : AsOutcome::OK;
  return {outcome, effective};
}

// ---- the refusal ---------------------------------------------------------

// Builds the EAUTH message. `what` is a noun phrase naming what was refused —
// "the built-in command 'reboot'", "built-in commands".
//
// Wording deliberately follows the modules' own refusals — mod_display.cpp's
// "backlight needs auth >= token; '%s' is at level %u", mod_hid.cpp's
// requireInjectAuth, mod_storage.cpp's requireAuth. A caller must not be able
// to tell a built-in's refusal from a module's, and the code (EAUTH) and the
// response shape are only half of that.
//
// It is kept SHORT ENOUGH TO FIT CmdError::msg (96 bytes) unaltered for every
// command name in the table: a message that truncated would be recognisable by
// its length. Truncates rather than overflowing if that ever stops being true.
inline size_t denyMessage(char *dst, size_t cap, const char *what, uint8_t need, const char *transport, uint8_t have) {
  if (dst == nullptr || cap == 0) {
    return 0;
  }
  int n = snprintf(dst, cap, "%s needs auth >= %s; transport '%s' is at level %u",
                   what != nullptr ? what : "this command", levelName(need), transport != nullptr ? transport : "?",
                   (unsigned)have);
  if (n < 0) {
    dst[0] = '\0';
    return 0;
  }
  return ((size_t)n < cap) ? (size_t)n : cap - 1;
}

}  // namespace CmdAuth
