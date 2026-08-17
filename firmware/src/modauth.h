// usbdongle W3 — the authorisation policy for MODULE modules and actions.
//
// The sibling of cmdauth.h, and deliberately built the same way. cmdauth.h is
// the policy for BUILT-IN commands (backlog S1); this is the policy for
// `{"mod":...,"act":...}` (backlog S6/S7/S8).
//
// DEPENDENCY-FREE ON PURPOSE, exactly like cmdauth.h / claims.h / modparam.h:
// <stddef.h>, <stdint.h>, <string.h> plus cmdauth.h and modparam.h, which are
// themselves dependency-free. That is not tidiness — the native test env sets
// build_src_filter = -<*>, so ANY policy that lived in mod_*.cpp could never be
// compiled, let alone asserted, on this machine. The whole decision table is
// therefore here, where `pio test -e native` can walk it.
//
// ---- WHY IT EXISTS (backlog S6, S7, S8) ---------------------------------
//
// After S1 the built-ins were gated centrally, but every MODULE still chose for
// itself whether to look at ctx.authLevel, and seven actions chose not to:
// led.set, led.auto, hid.status, storage.status, display.status, display.screen
// and display.refresh. Worse, Registry::dispatch() answered ENOMOD / EDISABLED /
// EREBOOT BEFORE any module was consulted, so the module map — and whether the
// dongle is currently a keyboard — was readable at AUTH_NONE. Nothing was
// exposed only because mod_http.cpp 401s before dispatching, which is exactly
// the "one transport is the whole security boundary" trap S1 removed for the
// built-ins. BLE dispatches at AUTH_NONE and would have walked straight into it.
//
// So the levels live HERE, in one .rodata table, and Registry::dispatch()
// enforces them in one place before the module's own dispatch runs. The
// property this buys is the same one S1 bought, and the one the BLE adapter is
// meant to rely on: A TRANSPORT MAY DISPATCH AT AUTH_NONE SAFELY.
//
// ---- THE LEVELS, and the rule that produced them ------------------------
//
//   reads state only ................................ TOKEN
//   changes device state / writes / injects ......... TOKEN
//   reveals or changes a SECRET, or changes what boots  PHYSICAL
//   nothing ......................................... NONE
//
// which comes out as: everything is TOKEN except storage.format, http.psk and
// http.pin, which are PHYSICAL and were PHYSICAL before this table existed.
// Two of stuart's standing decisions are visible by their ABSENCE from the
// PHYSICAL list: arming `hid` is `enable`, a TOKEN built-in (cmdauth.h), and the
// OTA upload/select path is AUTH_TOKEN in mod_http.cpp (backlog S9).
//
// ---- FAIL CLOSED --------------------------------------------------------
//
// Three separate mechanisms, because the failure is silent in every direction:
//
//   * A module or action MISSING FROM THE TABLE gets UNLISTED == PHYSICAL from
//     requiredFor()/moduleMinimum(). Each module file also carries a
//     static_assert that its rows exist, so this is normally a build failure —
//     the same belt-and-braces cmdauth.h uses.
//   * A ModuleAction or ModuleDescriptor whose minAuth FIELD was never
//     initialised holds 0. effective() maps anything below TOKEN to PHYSICAL,
//     so an aggregate initialiser that forgot the level closes the door rather
//     than opening it. There is deliberately NO WAY TO SPELL AUTH_NONE in a
//     descriptor: 0 means "undeclared", and if an open action is ever genuinely
//     wanted (backlog C2 argues for storage.caps) it must be added here as an
//     explicit sentinel, as a deliberate act, not by leaving a field blank.
//   * allGated() is a constexpr walk of a module's whole action table, so a
//     forgotten field is caught at BUILD time by one static_assert per module.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cmdauth.h"
#include "modparam.h"

namespace ModAuth {

// The same three values cmdauth.h mirrors from AuthLevel. Not redeclared: one
// set of constants for both policies is what makes "identical refusal" cheap.
constexpr uint8_t NONE = CmdAuth::NONE;
constexpr uint8_t TOKEN = CmdAuth::TOKEN;
constexpr uint8_t PHYSICAL = CmdAuth::PHYSICAL;

// What an UNLISTED module or action requires — PHYSICAL, i.e. a network caller
// is refused. See "FAIL CLOSED" above.
constexpr uint8_t UNLISTED = PHYSICAL;

// The floor applied to whatever a descriptor actually carries. 0 is what an
// aggregate initialiser leaves behind when the field was forgotten, and NONE
// has the same encoding, so both become PHYSICAL.
constexpr uint8_t effective(uint8_t declared) { return declared < TOKEN ? PHYSICAL : declared; }

// ---- the policy tables --------------------------------------------------

// "The level needed to learn this module EXISTS or to interact with it at all"
// (backlog S7). Checked in Registry::dispatch() before ENOMOD/EDISABLED/EREBOOT
// and in Registry::list() before a module is rendered, so a caller below it
// cannot see the module in a listing, cannot tell it from a module that does
// not exist, and cannot read its enabled state.
struct ModulePolicy {
  const char *mod;
  uint8_t minAuth;
};

// Every module in the image. TOKEN across the board today: which tools a device
// carries, and whether the keyboard is armed, are not public facts — but they
// are facts any holder of a session is entitled to.
constexpr ModulePolicy MODULES[] = {
    {"cdc", TOKEN},      // the console itself; `essential`, no actions
    {"led", TOKEN},      //
    {"display", TOKEN},  //
    {"hid", TOKEN},      // "is this dongle currently a keyboard" — S6 called this out by name
    {"storage", TOKEN},  //
    {"http", TOKEN},     //
};
constexpr size_t MODULE_COUNT = sizeof(MODULES) / sizeof(MODULES[0]);

struct ActionPolicy {
  const char *mod;
  const char *act;
  uint8_t minAuth;
};

// Every action of every module. The rule that produced each level is at the top
// of this file; the three PHYSICAL rows are the whole of the exception.
constexpr ActionPolicy ACTIONS[] = {
    // led — S8: the `led` built-in is TOKEN (CmdAuth::BUILTINS) and it is a pure
    // alias for led.set, so led.set must be TOKEN too or one of the two is a way
    // round the other. console.cpp carries a static_assert tying them together.
    {"led", "set", TOKEN},
    {"led", "auto", TOKEN},

    // hid — injection was already TOKEN by the module's own hand; `status` and
    // `release` were not, and `status` is the one S6 named: it reports armed /
    // bound / queue depth, i.e. whether this dongle is a keyboard right now.
    {"hid", "type", TOKEN},
    {"hid", "key", TOKEN},
    // Was reachable at ANY level as a "panic stop". It is still a state change
    // to the host's keyboard (it cancels a running job), and the rule admits no
    // AUTH_NONE actions, so it is TOKEN. Every transport that can start a job
    // can stop it.
    {"hid", "release", TOKEN},
    {"hid", "status", TOKEN},
    {"hid", "layout", TOKEN},

    // display — `screen` and `refresh` were ungated (backlog C7): switching to
    // `diag` HIDES the pairing PIN, which is an unauthenticated state change to
    // the one out-of-band channel this device has.
    {"display", "status", TOKEN},
    {"display", "backlight", TOKEN},
    {"display", "screen", TOKEN},
    {"display", "refresh", TOKEN},

    // storage — reads and writes are one tier at TOKEN (a directory listing of
    // someone's card is not public information); `format` erases a whole
    // filesystem and stays where it already was.
    {"storage", "caps", TOKEN},
    {"storage", "free", TOKEN},
    {"storage", "list", TOKEN},
    {"storage", "stat", TOKEN},
    {"storage", "read", TOKEN},
    {"storage", "verify", TOKEN},
    {"storage", "write", TOKEN},
    {"storage", "mkdir", TOKEN},
    {"storage", "delete", TOKEN},
    {"storage", "status", TOKEN},
    {"storage", "format", PHYSICAL},

    // http — the AP passphrase and the pairing PIN are the two secrets in the
    // image. Both READ and WRITE at PHYSICAL, unchanged: a stolen session token
    // must not become permanent access to the radio.
    {"http", "status", TOKEN},
    {"http", "psk", PHYSICAL},
    {"http", "pin", PHYSICAL},
    {"http", "sessions", TOKEN},
};
constexpr size_t ACTION_COUNT = sizeof(ACTIONS) / sizeof(ACTIONS[0]);

// ---- lookup -------------------------------------------------------------

// constexpr strcmp-for-equality, same shape as CmdAuth::streq (recursive so it
// is a constant expression under every -std this project builds with).
constexpr bool streq(const char *a, const char *b) {
  return (a == nullptr || b == nullptr) ? (a == b) : ((*a != *b) ? false : (*a == '\0' ? true : streq(a + 1, b + 1)));
}

constexpr uint8_t moduleMinimum(const char *mod) {
  for (size_t i = 0; i < MODULE_COUNT; i++) {
    if (streq(MODULES[i].mod, mod)) {
      return MODULES[i].minAuth;
    }
  }
  return UNLISTED;
}

constexpr bool isModuleListed(const char *mod) {
  for (size_t i = 0; i < MODULE_COUNT; i++) {
    if (streq(MODULES[i].mod, mod)) {
      return true;
    }
  }
  return false;
}

constexpr uint8_t requiredFor(const char *mod, const char *act) {
  for (size_t i = 0; i < ACTION_COUNT; i++) {
    if (streq(ACTIONS[i].mod, mod) && streq(ACTIONS[i].act, act)) {
      return ACTIONS[i].minAuth;
    }
  }
  return UNLISTED;
}

constexpr bool isListed(const char *mod, const char *act) {
  for (size_t i = 0; i < ACTION_COUNT; i++) {
    if (streq(ACTIONS[i].mod, mod) && streq(ACTIONS[i].act, act)) {
      return true;
    }
  }
  return false;
}

// The LOWEST level any MODULE accepts — TOKEN today.
//
// The module-side twin of CmdAuth::minimumLevel(), and used for the same
// pre-flight check: a caller below this cannot reach ANY module, so
// Registry::dispatch() refuses it before the id is looked up. Without that,
// an AUTH_NONE client could still tell ENOMOD from EAUTH and enumerate the
// module map of a device it has no session on.
//
// Registry::dispatch() derives the same number from the REGISTERED descriptors
// rather than calling this, so a module registered without a row here cannot
// widen the gate; this is the table's own answer, for the host tests.
constexpr uint8_t lowestModuleMinimum() {
  uint8_t lowest = PHYSICAL;
  for (size_t i = 0; i < MODULE_COUNT; i++) {
    uint8_t e = effective(MODULES[i].minAuth);
    if (e < lowest) {
      lowest = e;
    }
  }
  return lowest;
}

// ---- the three decisions, as functions ----------------------------------
//
// Named so the host tests assert the SHIPPED rule rather than a restatement of
// it, and so registry.cpp reads as policy rather than as arithmetic.

// May this caller see that `mod` exists at all? False == it must be omitted
// from list() and answered by dispatch() as if it did not exist.
constexpr bool moduleVisible(uint8_t have, uint8_t moduleMinAuth) {
  return CmdAuth::permits(have, effective(moduleMinAuth));
}

// May this caller run an action declared at `actionMinAuth` on a module
// declared at `moduleMinAuth`? Both bars, because the module's is a floor for
// everything inside it and an action is free to sit above it.
constexpr bool actionAllowed(uint8_t have, uint8_t moduleMinAuth, uint8_t actionMinAuth) {
  return moduleVisible(have, moduleMinAuth) && CmdAuth::permits(have, effective(actionMinAuth));
}

// ---- the whole of Registry::dispatch()'s gate, as one pure function ------
//
// registry.cpp CALLS THIS; it does not reimplement it. That is the only reason
// the host tests are worth anything — the native env cannot compile
// registry.cpp (ArduinoJson, FreeRTOS), so a decision table restated there
// would be tested here and enforced nowhere.
enum Decision : uint8_t {
  DECIDE_ALLOW = 0,         // hand it to the module
  DECIDE_EAUTH_ANY = 1,     // below the lowest bar of ANY module: refused WITHOUT a lookup being revealed
  DECIDE_ENOMOD = 2,        // no such module, and the caller is entitled to be told so
  DECIDE_EAUTH_MODULE = 3,  // the module exists but this caller may not know or touch it
  DECIDE_EAUTH_ACTION = 4,  // the module is reachable; this action is not
};

// ORDER IS THE SECURITY PROPERTY, so it is written once, here.
//
// DECIDE_EAUTH_ANY is tested FIRST and ignores `moduleExists` entirely: a
// caller below the lowest module bar gets byte-for-byte the same answer whether
// it named a real module or invented one, so it cannot enumerate the module map
// by the difference. That is exactly what Console::execute() does for the
// built-ins with CmdAuth::minimumLevel() (backlog S1), and what backlog S7 asks
// for here.
//
// Above that bar the caller holds a session, and ENOMOD is honest information —
// the same trade S1 makes when it answers EUNKNOWN to an authenticated caller.
//
// Both remaining EAUTH cases come BEFORE the module's state is consulted, which
// is why EDISABLED/EREBOOT are not in this enum: whether a module is enabled,
// or armed and waiting for a reboot, is a fact this function has already
// decided the caller is not entitled to.
constexpr Decision decide(uint8_t have, uint8_t lowestModuleMinAuth, bool moduleExists, uint8_t moduleMinAuth,
                          uint8_t actionMinAuth) {
  return !CmdAuth::permits(have, effective(lowestModuleMinAuth)) ? DECIDE_EAUTH_ANY
         : !moduleExists                                         ? DECIDE_ENOMOD
         : !CmdAuth::permits(have, effective(moduleMinAuth))     ? DECIDE_EAUTH_MODULE
         : !CmdAuth::permits(have, effective(actionMinAuth))     ? DECIDE_EAUTH_ACTION
                                                                 : DECIDE_ALLOW;
}

// ---- build-time invariants ----------------------------------------------

// Walks one module's action table. Called from a static_assert in each mod_*.cpp
// so a ModuleAction whose minAuth was never written is a BUILD failure, not a
// silent PHYSICAL at runtime.
constexpr bool allGated(const ModuleAction *actions, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (actions[i].minAuth < TOKEN) {
      return false;
    }
  }
  return true;
}

namespace detail {

constexpr bool everyActionModuleIsListed() {
  for (size_t i = 0; i < ACTION_COUNT; i++) {
    if (!isModuleListed(ACTIONS[i].mod)) {
      return false;
    }
  }
  return true;
}

constexpr bool everyActionIsAtLeastItsModule() {
  for (size_t i = 0; i < ACTION_COUNT; i++) {
    if (ACTIONS[i].minAuth < moduleMinimum(ACTIONS[i].mod)) {
      return false;
    }
  }
  return true;
}

constexpr bool nothingAtNone() {
  for (size_t i = 0; i < MODULE_COUNT; i++) {
    if (MODULES[i].minAuth < TOKEN) {
      return false;
    }
  }
  for (size_t i = 0; i < ACTION_COUNT; i++) {
    if (ACTIONS[i].minAuth < TOKEN) {
      return false;
    }
  }
  return true;
}

constexpr bool noDuplicateRows() {
  for (size_t i = 0; i < MODULE_COUNT; i++) {
    for (size_t j = i + 1; j < MODULE_COUNT; j++) {
      if (streq(MODULES[i].mod, MODULES[j].mod)) {
        return false;
      }
    }
  }
  for (size_t i = 0; i < ACTION_COUNT; i++) {
    for (size_t j = i + 1; j < ACTION_COUNT; j++) {
      if (streq(ACTIONS[i].mod, ACTIONS[j].mod) && streq(ACTIONS[i].act, ACTIONS[j].act)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace detail

// NOTHING AT AUTH_NONE. The hard requirement, asserted rather than reviewed.
static_assert(detail::nothingAtNone(), "a module or action sits at AUTH_NONE; nothing on this device may");
static_assert(detail::everyActionModuleIsListed(), "an action names a module with no row in ModAuth::MODULES");
// An action below its module's minimum would be unreachable-but-advertised: the
// module gate refuses first, so the lower number would be a lie in list().
static_assert(detail::everyActionIsAtLeastItsModule(), "an action sits BELOW its module's minimum, which can never apply");
static_assert(detail::noDuplicateRows(), "a duplicate row in ModAuth::MODULES/ACTIONS — the first one silently wins");
static_assert(lowestModuleMinimum() >= TOKEN, "some module is reachable below AUTH_TOKEN");
static_assert(UNLISTED == PHYSICAL, "the unlisted default must be the most restrictive level");
static_assert(effective(0) == PHYSICAL && effective(NONE) == PHYSICAL, "an undeclared level must fail closed");
static_assert(effective(TOKEN) == TOKEN && effective(PHYSICAL) == PHYSICAL, "effective() must not move a declared level");

// The three that were already PHYSICAL before this table existed and must not
// be lowered by it. Written out one by one, because "do not weaken anything at
// AUTH_PHYSICAL" is a requirement, not a preference.
static_assert(requiredFor("storage", "format") == PHYSICAL, "storage.format must stay AUTH_PHYSICAL");
static_assert(requiredFor("http", "psk") == PHYSICAL, "http.psk must stay AUTH_PHYSICAL");
static_assert(requiredFor("http", "pin") == PHYSICAL, "http.pin must stay AUTH_PHYSICAL");

}  // namespace ModAuth
