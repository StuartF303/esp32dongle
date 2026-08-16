// usbdongle W1 — module registry.
//
// This is the contract W2 (transports) and W3 (tool modules) both compile
// against, so the shape here matters more than what it currently holds. See
// ../ARCHITECTURE.md sections 2 and 4.
//
// A module is a static ModuleDescriptor plus up to four callbacks. It is
// registered once at boot and is enable/disable-able at runtime; the registry
// arbitrates the resources modules contend for (claims.h) and refuses
// impossible combinations with an error that names every blocker.
//
// Fixed-size tables, no dynamic allocation, descriptors stored by pointer —
// no PSRAM, 320 KB RAM, and 37 KB of it already went to TinyUSB.
//
// Threading: single-threaded by construction. Everything here runs from the
// cooperative Scheduler on the Arduino loop task. Transports (W2) that arrive
// on their own tasks — the HTTP server and NimBLE both will — MUST hand the
// request to the loop task rather than calling into the registry directly.
// There is no lock in here and adding one is a decision for stuart.

#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

#include "claims.h"

// ---- module callbacks ---------------------------------------------------

// Bring the module up / take it down. Return true on success; on failure set
// *errMsg to a short static string saying why (it is copied into the JSON
// response immediately, but a string literal is still the safe choice).
typedef bool (*ModuleLifecycleFn)(const char **errMsg);

// Handle one command aimed at this module. `act` is the action name, `p` the
// (possibly null/empty) params object; fill `d` and return true, or set
// *errCode / *errMsg and return false. Same contract as the console's
// built-in handlers, plus `act`. Only ever called while the module is enabled.
typedef bool (*ModuleDispatchFn)(const char *act, JsonObjectConst p, JsonObject d, const char **errCode,
                                 const char **errMsg);

// Optional module-specific state, rendered into the `modules` listing.
typedef void (*ModuleStatusFn)(JsonObject d);

struct ModuleDescriptor {
  const char *id;           // short, stable, wire-visible ("led", "hid", "msc")
  const char *name;         // human label for the UI
  const char *category;     // free-form grouping for the UI ("status", "input", ...)
  Claims::ClaimSet claims;  // build with Claims::claim(...) / Claims::none()
  ModuleLifecycleFn enable;
  ModuleLifecycleFn disable;
  ModuleDispatchFn dispatch;  // may be nullptr for a module with no actions
  ModuleStatusFn status;      // may be nullptr
};

// ---- results ------------------------------------------------------------

// Outcome of one enable()/disable() attempt. Deliberately fat: the caller must
// never have to guess what state things ended in, so this carries what was
// blocked, what was actually stopped, and a ready-built human message.
struct ModuleActionResult {
  static const uint8_t MAX_LIST = 12;  // == Registry::MAX_MODULES

  bool ok;
  bool changed;       // false + ok == the module was already in the requested state
  bool enabledAfter;  // the module's actual state when this returned
  const char *code;   // error code when !ok: ENOMOD / EBUSY / EENABLE / EDISABLE
  char msg[192];      // built here so every transport reports identical wording

  const char *blockedBy[MAX_LIST];  // enabled modules that prevent this enable
  uint8_t blockedByCount;
  const char *stopped[MAX_LIST];  // modules actually disabled by force
  uint8_t stoppedCount;
};

// What happened when the persisted enable-set was replayed at boot. A bad
// persisted state must never prevent boot, so nothing in here is fatal — it
// is recorded and reported through the `modules` command instead.
struct ModuleRestoreReport {
  static const uint8_t MAX_LIST = 12;

  bool nvsRead;    // false == nothing persisted yet (first boot), not an error
  bool nvsWriteOk; // false == a persist() call failed; state is live-only

  const char *restored[MAX_LIST];
  uint8_t restoredCount;
  const char *skipped[MAX_LIST];  // persisted, still registered, but no longer satisfiable
  uint8_t skippedCount;
  const char *unknown[MAX_LIST];  // persisted ids with no matching module (points into buf_)
  uint8_t unknownCount;
};

// ---- registry -----------------------------------------------------------

class Registry {
 public:
  static const uint8_t MAX_MODULES = 12;

  // `desc` must have static storage duration — the registry keeps the pointer.
  // Returns false if the table is full, desc is null, id is null/empty, or the
  // id is already registered.
  bool add(const ModuleDescriptor *desc);

  // Enable / disable by id. Both are no-op successes (ok=true, changed=false)
  // if the module is already in the requested state.
  //
  // enable(force=false) refuses a conflict, naming EVERY blocking module.
  // enable(force=true) disables the blockers first and reports exactly which
  // ones it stopped. Nothing is ever stopped without force.
  void enable(const char *id, bool force, ModuleActionResult &out);
  void disable(const char *id, ModuleActionResult &out);

  bool isEnabled(const char *id) const;

  // Renders the full descriptor list into `out`. This is the exact shape
  // GET /api/modules will serve, so keep it clean and stable.
  void list(JsonArray out) const;

  // Routes a command to a module. Sets ENOMOD (no such module) or EDISABLED
  // (registered but off) — both messages name the module — before calling the
  // module's own dispatch.
  bool dispatch(const char *id, const char *act, JsonObjectConst p, JsonObject d, const char **errCode,
                const char **errMsg);

  // Replays the persisted enable-set. Call once from setup(), after every
  // module has been add()ed. Enables in registration order, not NVS order, so
  // the outcome is deterministic.
  void restoreFromNvs();
  const ModuleRestoreReport &restoreReport() const { return report_; }

  uint8_t count() const { return count_; }
  const ModuleDescriptor *at(uint8_t i) const { return i < count_ ? mods_[i] : nullptr; }
  bool enabledAt(uint8_t i) const { return i < count_ && enabled_[i]; }
  int8_t indexOf(const char *id) const;

 private:
  // Longest "id1,id2,..." string we will persist. 12 modules x 15 chars + NUL.
  static const size_t PERSIST_BUF_SIZE = 192;

  bool startModule(uint8_t idx, const char **errMsg);
  bool stopModule(uint8_t idx, const char **errMsg);
  void persist();

  const ModuleDescriptor *mods_[MAX_MODULES] = {nullptr};
  bool enabled_[MAX_MODULES] = {false};
  uint8_t count_ = 0;

  bool persistSuppressed_ = false;
  char buf_[PERSIST_BUF_SIZE] = {0};  // holds the raw NVS string; report_.unknown points into it
  ModuleRestoreReport report_ = {};
  char errBuf_[96] = {0};  // ENOMOD/EDISABLED message for dispatch()
};

// One registry for the whole image, matching `scheduler`.
extern Registry registry;
