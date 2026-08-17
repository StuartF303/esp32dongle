// usbdongle W1 — machine-readable parameter descriptors for module actions.
//
// WHY THIS EXISTS. ModuleAction::params used to be one free-text string, e.g.
//   rgb:"rrggbb"|"off"
// which is a sentence, not a schema. ARCHITECTURE.md section 2 promises "the
// web UI renders itself from the descriptor — so a new module needs ZERO
// front-end changes", and prose cannot drive a form. So the UI fell back to a
// single JSON textbox whose PLACEHOLDER was that prose: it advertised
//   rgb:"rrggbb"|"off"
// and then rejected everything that was not `{"rgb":"ff0000"}` with "bad JSON
// for led.set". The field advertised one syntax and demanded another. That is
// the defect this header removes.
//
// DEPENDENCY-FREE ON PURPOSE, exactly like claims.h: <stddef.h>, <stdint.h>
// and <string.h> only — no Arduino.h, no ArduinoJson — so `pio test -e native`
// compiles the enum-splitting and the type naming on the host. Keep it that
// way; the JSON emission lives in registry.cpp, which is not host-buildable.
//
// EVERYTHING HERE IS .rodata AND ALLOCATION-FREE. A ModuleParam is four
// pointers, an enum, a bool and two int32s of static const data per parameter;
// a module's whole table costs flash and not one byte of RAM.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// What a parameter IS, in the only terms a form can render.
//
// P_ENUM_LIST is the fifth type and it was added rather than fudged: `hid.key`
// takes p.mods as a JSON ARRAY of modifier names, and the four types alone
// could only have described it as a string — which the module would then
// silently ignore (JsonArrayConst of a string is null, so the modifiers would
// vanish with no error). Describing it honestly costs one type and one
// checkbox group in the UI.
enum ParamType : uint8_t {
  P_STRING = 0,
  P_INT = 1,
  P_BOOL = 2,
  P_ENUM = 3,       // one of enumVals
  P_ENUM_LIST = 4,  // a JSON array of zero or more of enumVals
};

struct ModuleParam {
  const char *name;      // the key inside `p`, exactly as the dispatch reads it
  ParamType type;
  bool required;         // false == the dispatch has a defined behaviour without it
  const char *help;      // short, per-parameter; says what an ALTERNATIVE param does to this one
  const char *enumVals;  // comma-separated, for P_ENUM / P_ENUM_LIST; nullptr otherwise
  int32_t min, max;      // P_INT only; ModParam::NO_MIN / NO_MAX == unbounded
};

namespace ModParam {

// Sentinels for "the dispatch imposes no bound on this side". Emitted as an
// absent "min"/"max" rather than as INT32_MIN, which a UI would render as a
// spinner bound nobody meant.
constexpr int32_t NO_MIN = INT32_MIN;
constexpr int32_t NO_MAX = INT32_MAX;

// Longest single enum value the emitter will render. Nothing in the tree is
// close (the longest is "en_GB"), but an over-long value is REPORTED as
// truncated rather than silently shortened — see Registry::list().
constexpr size_t MAX_ENUM_VALUE = 23;

// Wire name of a type. An unrecognised value is "unknown", NOT "string": a UI
// that renders an unknown type as a text box would silently mistype whatever a
// future ParamType means.
inline const char *typeName(uint8_t t) {
  switch (t) {
    case P_STRING:
      return "string";
    case P_INT:
      return "int";
    case P_BOOL:
      return "bool";
    case P_ENUM:
      return "enum";
    case P_ENUM_LIST:
      return "enum_list";
    default:
      return "unknown";
  }
}

// Number of NON-EMPTY values in a comma-separated list. Empty entries ("a,,b")
// are skipped rather than counted, so the count and enumValueAt() index the
// same set.
inline uint8_t enumCount(const char *csv) {
  if (csv == nullptr) {
    return 0;
  }
  uint8_t n = 0;
  const char *cur = csv;
  while (*cur != '\0') {
    const char *comma = strchr(cur, ',');
    size_t len = (comma != nullptr) ? (size_t)(comma - cur) : strlen(cur);
    if (len > 0) {
      n++;
    }
    if (comma == nullptr) {
      break;
    }
    cur = comma + 1;
  }
  return n;
}

// Copies value `index` (0-based, over the non-empty values) into `out`,
// bounded and always NUL-terminated.
//
// RETURNS THE VALUE'S FULL LENGTH, not the number of bytes copied, so a caller
// can DETECT truncation instead of shipping a silently shortened enum value.
// Returns 0 if the index is past the end.
inline size_t enumValueAt(const char *csv, uint8_t index, char *out, size_t cap) {
  if (out != nullptr && cap > 0) {
    out[0] = '\0';
  }
  if (csv == nullptr) {
    return 0;
  }
  uint8_t n = 0;
  const char *cur = csv;
  while (*cur != '\0') {
    const char *comma = strchr(cur, ',');
    size_t len = (comma != nullptr) ? (size_t)(comma - cur) : strlen(cur);
    if (len > 0) {
      if (n == index) {
        if (out != nullptr && cap > 0) {
          size_t copy = (len < cap - 1) ? len : cap - 1;
          memcpy(out, cur, copy);
          out[copy] = '\0';
        }
        return len;
      }
      n++;
    }
    if (comma == nullptr) {
      break;
    }
    cur = comma + 1;
  }
  return 0;
}

// ---- constructors -------------------------------------------------------
//
// constexpr, so a table built from them is still a .rodata aggregate — the
// point is readability at the call site, not indirection. Written as
// functions rather than macros so a wrong argument is a type error.

constexpr ModuleParam str(const char *name, bool required, const char *help) {
  return ModuleParam{name, P_STRING, required, help, nullptr, NO_MIN, NO_MAX};
}

constexpr ModuleParam num(const char *name, bool required, const char *help, int32_t min = NO_MIN,
                          int32_t max = NO_MAX) {
  return ModuleParam{name, P_INT, required, help, nullptr, min, max};
}

constexpr ModuleParam flag(const char *name, bool required, const char *help) {
  return ModuleParam{name, P_BOOL, required, help, nullptr, NO_MIN, NO_MAX};
}

constexpr ModuleParam choice(const char *name, bool required, const char *help, const char *values) {
  return ModuleParam{name, P_ENUM, required, help, values, NO_MIN, NO_MAX};
}

constexpr ModuleParam choiceList(const char *name, bool required, const char *help, const char *values) {
  return ModuleParam{name, P_ENUM_LIST, required, help, values, NO_MIN, NO_MAX};
}

}  // namespace ModParam

// One action a module accepts. Static, .rodata, zero RAM. This is what makes
// ARCHITECTURE.md section 2's "the web UI renders itself from GET
// /api/modules, a new module needs zero front-end changes" true rather than
// aspirational: without it the listing says a module exists but nothing about
// what it DOES, so every module still needs a hand-written panel.
//
// `params` is a MACHINE-READABLE table (above), not a prose sketch. It used to
// be one free-text string and the UI could do nothing with it but put it in a
// placeholder over a raw-JSON box — see the defect described at the top of this
// header. The rule for filling it in: read the dispatch handler and describe
// what it ACTUALLY accepts, including which parameters are genuinely optional.
// The prose it replaced had already drifted from the code in six places.
//
// IT LIVES HERE, not in registry.h, so that modauth.h — which is
// dependency-free and therefore host-testable — can walk a real module's action
// table in a static_assert and in `pio test -e native`. registry.h includes
// this header, so nothing else moved.
struct ModuleAction {
  const char *act;            // "set", "type", "scan"
  const char *help;           // one line, imperative, for a tooltip or `help` output
  const ModuleParam *params;  // static .rodata table; nullptr for an action with no params
  uint8_t paramCount;
  // The AuthLevel a caller must hold for this action to run (backlog S6).
  // Enforced CENTRALLY in Registry::dispatch(), before the module's own
  // dispatch is called, so no module checks auth for itself and none of them
  // can forget to. Initialise it from ModAuth::requiredFor(mod, act) — never
  // from a literal — so modauth.h stays the single source of truth.
  //
  // 0 means UNDECLARED, not AUTH_NONE: ModAuth::effective() maps it to
  // AUTH_PHYSICAL, and ModAuth::allGated() turns a forgotten field into a build
  // failure. There is no way to spell "open to everyone" here, on purpose.
  uint8_t minAuth;
};

// Fills a ModuleAction's `params` + `paramCount` pair from one table.
//
// A macro, deliberately, and the only one in this header: the two fields must
// agree, and a hand-written count that drifts from its table walks off the end
// of .rodata into whatever follows — a bug that shows up as garbage parameter
// names in the web UI and nowhere else.
#define MOD_PARAMS(table) (table), (uint8_t)(sizeof(table) / sizeof((table)[0]))
