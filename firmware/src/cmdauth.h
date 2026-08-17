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
inline bool permits(uint8_t have, uint8_t need) { return have >= need; }

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
    // The only one above TOKEN. A phone with a valid session can do a great
    // deal to this device, but it cannot take it away from the person at the
    // desk.
    {"reboot", PHYSICAL},
};
constexpr size_t BUILTIN_COUNT = sizeof(BUILTINS) / sizeof(BUILTINS[0]);

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
