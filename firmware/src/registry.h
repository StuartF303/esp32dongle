// usbdongle W1 — module registry.
//
// This is the contract W2 (transports) and W3 (tool modules) both compile
// against, so the shape here matters more than what it currently holds. See
// ../ARCHITECTURE.md sections 2 and 4.
//
// A module is a static ModuleDescriptor plus up to five callbacks. It is
// registered once at boot and is enable/disable-able at runtime; the registry
// arbitrates the resources modules contend for (claims.h), refuses impossible
// combinations with an error that names every blocker, owns the module's
// periodic task, and persists which modules the user wants running.
//
// Fixed-size tables, no dynamic allocation, descriptors stored by pointer —
// no PSRAM, 320 KB RAM, and 37 KB of it already went to TinyUSB.
//
// ---- THREADING ---------------------------------------------------------
//
// The public API is guarded by a RECURSIVE FreeRTOS mutex, created by
// begin(). Call begin() before the first add(); if you do not, every guard
// degrades to a no-op and you are back to "single-threaded by convention".
// The lock is held across the module's own enable()/disable()/dispatch()/
// status() callbacks, which is what turns "status() is only called while the
// module is enabled" from an aspiration into an invariant.
//
// It is recursive because a module callback legitimately re-enters read-only
// registry calls (isEnabled(), indexOf()). It must NOT re-enter the mutating
// ones — a disable() that calls registry.enable() would corrupt the very
// iteration that called it — so those carry a separate reentrancy flag and
// return EREENTRANT instead of deadlocking or misbehaving.
//
// The inline accessors at the bottom (count/at/enabledAt/restoreReport) are
// deliberately unguarded: they are boot-time and diagnostic callers, and
// guarding them would mean taking a mutex inside a loop that already holds it.
//
// ---- TinyUSB MODULES: READ THIS BEFORE WRITING hid OR msc --------------
//
// A module that owns a TinyUSB interface CANNOT be started at runtime, and no
// amount of registry design changes that. USBHID / USBMSC register their
// interface descriptors from their CONSTRUCTORS;
// `tinyusb_enable_interface2()` returns ESP_FAIL once `tinyusb_init()` has run
// (esp32-hal-tinyusb.c:817), and that happens inside USB.begin(), called from
// app_main because ARDUINO_USB_CDC_ON_BOOT=1. Verified boot order on this
// toolchain, read out of the linked image (see registry.cpp for the trace):
//
//   do_global_ctors()  ->  app_main(): Serial.begin(), USB.begin()  [FROZEN]
//                      ->  initArduino(): nvs_flash_init()
//                      ->  loopTask: setup()
//
// The descriptor set is frozen before setup() — before a single line of OUR
// code outside a static constructor executes.
//
// Therefore such a module MUST:
//   1. construct its USBHID/USBMSC object at FILE SCOPE, guarded by
//      ModulePersist::wasEnabledAtBoot("<id>") so the interface only exists
//      when the user has explicitly armed it (stuart's decision, 2026-08-16:
//      HID is reboot-gated, not "on by default and idle");
//   2. set `bootTimeBinding = true` in its descriptor, which makes runtime
//      enable()/disable() record the intent and report pendingRestart instead
//      of pretending to start hardware that cannot be started;
//   3. have its enable() VERIFY the interface is actually present — the object
//      exists, the descriptor was accepted — and FAIL LOUDLY with a specific
//      error if not. Returning true from an enable() that bound nothing is the
//      worst outcome available: the UI says "on", the host sees no keyboard,
//      and there is no error anywhere to explain it.

#pragma once

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "claims.h"
#include "modparam.h"

// ---- command context ----------------------------------------------------

// Who is asking. Passed to every dispatch so a module can apply its own
// policy instead of each transport adapter reinventing one — `hid.type`
// injecting keystrokes into the host PC is not a thing an unauthenticated
// Wi-Fi client should be able to do, and the module is the only place that
// knows that about itself.
enum AuthLevel : uint8_t {
  AUTH_NONE = 0,      // unauthenticated: a fresh WS/BLE client, pre-token
  AUTH_TOKEN = 1,     // presented a valid session token (see ARCHITECTURE §4 auth)
  AUTH_PHYSICAL = 2,  // physically attached: USB CDC. Cable == consent.
};

struct CmdContext {
  const char *transport;  // "cdc", "ws", "ble" — for logging and policy, never for behaviour a module fakes
  uint8_t authLevel;      // an AuthLevel
  uint32_t reqId;         // the request's "id", 0 if it had none. For correlating a later ACCEPTED completion event.
};

// ---- results ------------------------------------------------------------

// What a dispatch did. Three outcomes, not two: `hid.type` of a long macro is
// ~8 ms per two reports and would block the cooperative scheduler for tens of
// seconds if it ran to completion inline, so it must be able to say "queued".
enum DispatchResult : uint8_t {
  DISPATCH_OK = 0,        // done; `d` holds the result
  DISPATCH_FAIL = 1,      // failed; `err` holds code+msg. `d` may still hold partial data.
  DISPATCH_ACCEPTED = 2,  // queued; `d` describes a job handle, completion arrives later as an event
};

// Caller-owned error detail. Caller-owned on purpose: the previous
// `const char **errCode, const char **errMsg` pair forced anything wanting a
// formatted message to own a static scratch buffer that outlived the call
// (the registry had errBuf_; every future module would have needed its own).
// One struct on the caller's stack removes that from the contract entirely.
struct CmdError {
  const char *code;  // short, stable, wire-visible: EARGS / EBUSY / EREBOOT / ...
  char msg[96];      // formatted here; copied into the response before this goes out of scope
};

// Sets code and a printf-formatted message on `err`, tolerating err == nullptr.
// Truncates rather than overflowing.
void cmdErrorf(CmdError *err, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

// ---- module callbacks ---------------------------------------------------

// Bring the module up / take it down. Return true on success; on failure set
// *errMsg to a short static string saying why (it is copied into the JSON
// response immediately, but a string literal is still the safe choice).
typedef bool (*ModuleLifecycleFn)(const char **errMsg);

// Handle one command aimed at this module. `ctx` is the caller, `act` the
// action name, `p` the (possibly null/empty) params object. Fill `d` and
// return DISPATCH_OK, fill `d` with a job handle and return DISPATCH_ACCEPTED,
// or fill `err` and return DISPATCH_FAIL — in which case anything already
// written to `d` is still sent, so a partial result (a truncated scan, a
// listing with unreadable entries) survives its own error.
//
// Only ever called while the module is enabled, and with the registry lock
// held — so a dispatch must not block for long and must not call back into
// Registry::enable()/disable()/dispatch() (it will get EREENTRANT).
typedef DispatchResult (*ModuleDispatchFn)(const CmdContext &ctx, const char *act, JsonObjectConst p, JsonObject d,
                                           CmdError *err);

// Optional module-specific state, rendered into the `modules` listing.
typedef void (*ModuleStatusFn)(JsonObject d);

// Optional periodic work, registered with the Scheduler by the registry and
// called ONLY while the module is enabled. Modules do not register their own
// scheduler tasks and do not keep their own copy of the enabled flag — one
// source of truth, and it is enabled_[i] in here.
typedef void (*ModuleTaskFn)();

// One action a module accepts. Static, .rodata, zero RAM. This is what makes
// ARCHITECTURE.md section 2's "the web UI renders itself from GET
// /api/modules, a new module needs zero front-end changes" true rather than
// aspirational: without it the listing says a module exists but nothing about
// what it DOES, so every module still needs a hand-written panel.
//
// `params` is a MACHINE-READABLE table (modparam.h), not a prose sketch. It
// used to be one free-text string and the UI could do nothing with it but put
// it in a placeholder over a raw-JSON box — see the defect described at the
// top of modparam.h. The rule for filling it in: read the dispatch handler and
// describe what it ACTUALLY accepts, including which parameters are genuinely
// optional. The prose it replaced had already drifted from the code in six
// places.
struct ModuleAction {
  const char *act;             // "set", "type", "scan"
  const char *help;            // one line, imperative, for a tooltip or `help` output
  const ModuleParam *params;   // static .rodata table; nullptr for an action with no params
  uint8_t paramCount;
};

struct ModuleDescriptor {
  const char *id;           // short, stable, wire-visible ("led", "hid", "msc"); <= Registry::MAX_ID_LEN
  const char *name;         // human label for the UI
  const char *category;     // free-form grouping for the UI ("status", "transport", "input", ...)
  Claims::ClaimSet claims;  // build with Claims::claim(...) / Claims::none()

  // Default state on a device that has never heard of this module. Applied by
  // restoreFromNvs() on a virgin NVS AND to a module that is absent from the
  // persisted KNOWN set — i.e. one newly added to the firmware (modset.h).
  // It is NOT applied to a module the owner has explicitly turned off: that
  // one is in the known set and absent from the enabled set, and it stays off.
  // Lives here rather than as a hardcoded id in main.cpp so that "is this on
  // out of the box?" is answered in the module's own file. `hid` must never
  // set this.
  bool defaultEnabled;

  // The module's real gate is boot, not runtime: it owns a USB interface whose
  // descriptor was frozen before setup() ran. See the TinyUSB block at the top
  // of this file. Runtime enable()/disable() on such a module records intent
  // and reports pendingRestart; it does not call enable()/disable().
  bool bootTimeBinding;

  // This module may not be disabled and is force-enabled at every boot
  // regardless of NVS. Exactly one thing needs it today — `cdc`, which is the
  // only control link the device has, and which a `force` enable would
  // otherwise be free to stop out from under the reply it is about to send.
  bool essential;

  ModuleLifecycleFn enable;   // may be nullptr
  ModuleLifecycleFn disable;  // may be nullptr
  ModuleDispatchFn dispatch;  // may be nullptr for a module with no actions
  ModuleStatusFn status;      // may be nullptr

  const ModuleAction *actions;  // may be nullptr
  uint8_t actionCount;

  ModuleTaskFn tick;        // may be nullptr
  uint32_t tickIntervalMs;  // 0 == every scheduler pass
};

// Outcome of one enable()/disable() attempt. Deliberately fat: the caller must
// never have to guess what state things ended in, so this carries what was
// blocked, what was actually stopped, what refused to stop, and a ready-built
// human message.
struct ModuleActionResult {
  static const uint8_t MAX_LIST = 12;  // >= Registry::MAX_MODULES, asserted below

  bool ok;
  bool changed;         // false + ok == the module was already in the requested state
  bool enabledAfter;    // the module's actual LIVE state when this returned
  bool pendingRestart;  // the change is recorded but takes effect at the next boot (bootTimeBinding)
  const char *code;     // ENOMOD / EBUSY / EENABLE / EDISABLE / EESSENTIAL / EREENTRANT, or EREBOOT with ok==true
  char msg[192];        // built here so every transport reports identical wording

  const char *blockedBy[MAX_LIST];  // enabled modules that prevent this enable
  uint8_t blockedByCount;
  const char *stopped[MAX_LIST];  // modules force actually disabled, cleanly
  uint8_t stoppedCount;
  // Modules whose disable() FAILED during a force. They are no longer holding
  // claims (see the stance in stopModule) but their hardware state is unknown.
  // Split out of stopped[] because reporting a failed teardown as "stopped"
  // tells the operator the opposite of what happened.
  const char *failedToStop[MAX_LIST];
  uint8_t failedToStopCount;
};

// What happened when the persisted enable-set was replayed at boot. A bad
// persisted state must never prevent boot, so nothing in here is fatal — it
// is recorded and reported through the `modules` command instead.
struct ModuleRestoreReport {
  static const uint8_t MAX_LIST = 12;

  bool nvsRead;     // false == nothing persisted yet (first boot), not an error
  bool nvsWriteOk;  // false == a persist() call failed; state is live-only
  // false == the KNOWN-module set is absent: a virgin device, or one upgraded
  // from a build that never wrote one. In the latter case every registered
  // module is treated as known for that boot, so nothing the owner disabled is
  // resurrected — see modset.h. It is written at the end of this restore, so
  // it is false at most once per device.
  bool knownRead;
  // The stored string was longer than buf_, so Preferences::getString()
  // returned 0 and left the buffer EMPTY. Without this flag that is
  // indistinguishable from "nothing is enabled": every module silently off,
  // no error anywhere, and the persisted set still unreadable next boot.
  bool nvsTooLong;
  size_t nvsStoredLen;  // NVS-reported length of the stored value, including its NUL. 0 == key absent.

  const char *restored[MAX_LIST];
  uint8_t restoredCount;
  const char *skipped[MAX_LIST];  // persisted, still registered, but no longer satisfiable
  uint8_t skippedCount;
  const char *unknown[MAX_LIST];  // persisted ids with no matching module (points into buf_)
  uint8_t unknownCount;
  // Modules this device had never heard of, which therefore took their
  // descriptor's defaultEnabled. Reported because "why did that turn itself
  // on?" must have an answer that is not "read the source".
  const char *defaulted[MAX_LIST];
  uint8_t defaultedCount;
  // bootTimeBinding modules the user has armed. Whether one actually BOUND is
  // decided by its own static-constructor check, not by anything in here.
  const char *armed[MAX_LIST];
  uint8_t armedCount;
};

// ---- persisted state, readable WITHOUT a Registry -----------------------

namespace ModulePersist {

// Was module `id` in the persisted enabled-set at the last write?
//
// EXISTS FOR ONE REASON: a TinyUSB module has to decide whether to construct
// its interface at FILE SCOPE — before setup(), before Registry::add() has
// ever been called, before the global registry object is safe to touch. This
// reads the same NVS namespace/key directly and answers without constructing
// or consulting the Registry at all.
//
// !! SHARED FORMAT !! This and Registry::persist() are two readers of one
// on-flash format ("id1,id2,..." in namespace "modreg", key "on"). Change one
// and you must change the other; there is no compiler check that spans them.
// The keys live in registry.cpp and the parser in modset.h, which both use.
//
// It reads the ENABLED set only. The companion "known" key (modset.h) is a
// boot-decision input for the Registry and means nothing here: a TinyUSB
// module asks "did the user arm me", and the answer is the enabled set.
//
// Returns false if NVS is unreadable, the key is absent, or the stored value
// is too long to parse — i.e. it fails CLOSED. For `hid` that means "a
// corrupt NVS gives you no keyboard", which is the correct direction.
//
// Calling it from a static constructor is the INTENDED use and the only one
// that is early enough. NVS is not initialised at that point in the boot, so
// this initialises it itself; see the traced boot order in registry.cpp.
bool wasEnabledAtBoot(const char *id);

}  // namespace ModulePersist

// ---- registry -----------------------------------------------------------

class Registry {
 public:
  static const uint8_t MAX_MODULES = 12;
  // Longest module id. Wire-visible, persisted, and budgeted for in
  // PERSIST_BUF_SIZE — enforced in add() rather than left to a code review.
  static const size_t MAX_ID_LEN = 15;
  // Longest "id1,id2,..." string we will persist. Public because
  // ModulePersist::wasEnabledAtBoot() reads the same string with the same
  // buffer size and must not carry its own copy of the number.
  //
  // UNCHANGED at 192 by the addition of the KNOWN set (modset.h): that is a
  // second NVS key of the same shape, whose worst case is the same 12 ids, so
  // the same bound covers both. It is a stack buffer in restoreFromNvs(), not
  // a second member, so the second list costs no static RAM either.
  static const size_t PERSIST_BUF_SIZE = 192;
  // 12 ids x 15 chars + 11 commas + NUL == 192, exactly. If MAX_MODULES or
  // MAX_ID_LEN grows, the persisted set silently truncates and modules quietly
  // stop coming back after a reboot. Fail the build instead.
  static_assert(PERSIST_BUF_SIZE >= MAX_MODULES * (MAX_ID_LEN + 1), "PERSIST_BUF_SIZE cannot hold a full module set");

  // Creates the lock. Call ONCE from setup(), before the first add().
  // Idempotent. If it is never called the registry still works, but with no
  // mutual exclusion at all.
  void begin();

  // `desc` must have static storage duration — the registry keeps the pointer.
  // Also registers desc->tick with the global scheduler, if it has one.
  //
  // Returns false if: the table is full, desc is null, id is null/empty/longer
  // than MAX_ID_LEN, the id is already registered, the scheduler is full, or
  // the registry has been SEALED by restoreFromNvs(). Check the return — a
  // module that silently failed to register is a module whose absence is only
  // discovered by a user wondering where it went.
  bool add(const ModuleDescriptor *desc);

  // Enable / disable by id. Both are no-op successes (ok=true, changed=false)
  // if the module is already in the requested state.
  //
  // enable(force=false) refuses a conflict, naming EVERY blocking module.
  // enable(force=true) disables the blockers first and reports exactly which
  // ones it stopped. Nothing is ever stopped without force, and an `essential`
  // module is never stopped at all.
  //
  // On a bootTimeBinding module neither call touches hardware: the intent is
  // persisted and out.pendingRestart is set.
  void enable(const char *id, bool force, ModuleActionResult &out);
  void disable(const char *id, ModuleActionResult &out);

  // Live state, not persisted intent. Non-const because it takes the lock.
  bool isEnabled(const char *id);

  // Renders the full descriptor list into `out`. This is the exact shape
  // GET /api/modules will serve, so keep it clean and stable.
  void list(JsonArray out);

  // Renders ONE module's status() into `out`. Returns false if there is no
  // such module, it is disabled, or it has no status callback — the same
  // "status() is only called while the module is enabled" invariant list()
  // holds, under the same lock.
  //
  // EXISTS SO THAT MODULES CAN READ EACH OTHER WITHOUT INCLUDING EACH OTHER.
  // `display` has to render whether the AP is up, its SSID, its IP and its
  // client count. The alternatives were: include mod_http.h and read its
  // file-scope state (a direct dependency between two W3 modules, and a second
  // copy of the truth that can disagree with what the phone is shown), or call
  // list() and throw away eleven twelfths of a ~4 KB document twice a second.
  // This takes the same lock, honours the same invariant, and costs one
  // module's worth of JSON.
  bool statusOf(const char *id, JsonObject out);

  // Routes a command to a module. Answers ENOMOD (no such module), EDISABLED
  // (registered but off), EREBOOT (armed but not bound until the next boot),
  // ENOACT (no dispatch / no act) itself, all naming the module, before the
  // module's own dispatch is ever reached.
  DispatchResult dispatch(const char *id, const char *act, const CmdContext &ctx, JsonObjectConst p, JsonObject d,
                          CmdError *err);

  // Replays the persisted enable-set, applying descriptor defaults on a first
  // boot. Call once from setup(), after every module has been add()ed.
  // Enables in registration order, not NVS order, so the outcome is
  // deterministic. SEALS the registry: add() fails afterwards.
  void restoreFromNvs();
  const ModuleRestoreReport &restoreReport() const { return report_; }

  // Unguarded, boot-time/diagnostic accessors — see the threading note above.
  uint8_t count() const { return count_; }
  const ModuleDescriptor *at(uint8_t i) const { return i < count_ ? mods_[i] : nullptr; }
  bool enabledAt(uint8_t i) const { return i < count_ && enabled_[i]; }
  int8_t indexOf(const char *id) const;

  // Scheduler trampoline target. Public because the trampoline table in
  // registry.cpp is at namespace scope; not part of the module contract.
  void tickAt(uint8_t i);

 private:
  bool startModule(uint8_t idx, const char **errMsg);
  bool stopModule(uint8_t idx, const char **errMsg);
  void persist();
  // Records the set of ids this firmware registers. See modset.h.
  void persistKnown(const char *current);
  // THE arbitration query, used by both enable() (to refuse) and list() (to
  // render blocked_by). One implementation, so the rule a UI shows and the
  // rule the device applies cannot drift apart.
  uint8_t blockersOf(uint8_t idx, const char **ids, uint8_t maxIds, char *why, size_t whySize) const;

  const ModuleDescriptor *mods_[MAX_MODULES] = {nullptr};
  // LIVE: the module is running and holds its claims. Claims are held iff this
  // is true — there is no separate claim table, so a claim cannot be leaked.
  bool enabled_[MAX_MODULES] = {false};
  // INTENT: what gets persisted and replayed next boot. Identical to enabled_
  // for every ordinary module; they diverge only for a bootTimeBinding module
  // that has been armed or disarmed since the last reboot.
  bool desired_[MAX_MODULES] = {false};
  uint8_t count_ = 0;

  SemaphoreHandle_t lock_ = nullptr;
  bool inCall_ = false;   // a mutating public call is in progress (reentrancy guard)
  bool restoring_ = false;  // inside restoreFromNvs(): suppresses persist(), and boot binding is real
  bool sealed_ = false;     // restoreFromNvs() has run; no more add()

  char buf_[PERSIST_BUF_SIZE] = {0};  // holds the raw NVS string; report_.unknown points into it
  ModuleRestoreReport report_ = {};
};

// These live after the class because ModuleActionResult and
// ModuleRestoreReport are declared before it and cannot name Registry.
//
// The failure mode being prevented is UNDER-REPORTING, which is worse than it
// sounds: the force loop stops every conflicting module regardless of these
// caps, but stopped[] stops recording once full — so the response would
// understate what was actually changed on the device, and a UI reconciling
// against it would show modules as running that are not.
static_assert(ModuleActionResult::MAX_LIST >= Registry::MAX_MODULES,
              "ModuleActionResult::MAX_LIST would under-report blocked/stopped modules");
static_assert(ModuleRestoreReport::MAX_LIST >= Registry::MAX_MODULES,
              "ModuleRestoreReport::MAX_LIST would under-report the boot restore");

// One registry for the whole image, matching `scheduler`.
extern Registry registry;
