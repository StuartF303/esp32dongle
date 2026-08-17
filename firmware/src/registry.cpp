#include "registry.h"

#include <Preferences.h>
#include <nvs_flash.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "modset.h"
#include "scheduler.h"

Registry registry;

namespace {

// ---- persisted format ----------------------------------------------------
//
// NVS storage for the enabled-module set. One string key, "id1,id2,...",
// rather than a bitmask over registration indices: a bitmask silently means
// something different the moment modules are reordered or one is removed,
// which is exactly the "code changed" case this has to survive. It is also
// why splitting Claims::RES_RADIO cost no migration — resource indices are
// never persisted.
//
// !! TWO READERS !! Registry::restoreFromNvs() and
// ModulePersist::wasEnabledAtBoot() both parse this, and the latter runs
// before the Registry exists (file-scope TinyUSB construction — see the
// header). Both live in this file so the format has one home; if you change
// the separator, the key or the namespace, change both functions below.
//
// This lives in the 32 K `nvs` partition at 0x9000 (ARCHITECTURE.md section 3;
// the 4 K `nvs_keys` at 0x11000 is reserved-but-unused, for NVS encryption
// later). That partition also has to carry Wi-Fi STA config, an auth PIN and a
// rotating session token, and NVS needs spare pages for compaction — so one
// short string here, not a key per module.
//
// TWO KEYS, and the second one is not a nicety:
//   "on"    — the ids that are ENABLED (what the owner asked for).
//   "known" — every id REGISTERED at the last boot.
// Without the second, "absent from the enabled set" cannot be told apart from
// "this firmware has a module the device has never seen", so a newly added
// default-on module was off forever. The rule is ModSet::wantAtBoot(); the
// reasoning and the migration stance are in modset.h. Worst case is the same
// 12 ids as "on", so both fit PERSIST_BUF_SIZE and the nvs partition grows by
// at most another ~200 bytes.
const char *NVS_NAMESPACE = "modreg";
const char *NVS_KEY = "on";
const char *NVS_KEY_KNOWN = "known";

// Appends to a bounded buffer at *pos. Silently truncates rather than
// overflowing; every caller here is building a human-readable message.
void appendf(char *buf, size_t size, size_t *pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void appendf(char *buf, size_t size, size_t *pos, const char *fmt, ...) {
  if (*pos >= size - 1) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf + *pos, size - *pos, fmt, args);
  va_end(args);
  if (n < 0) {
    return;
  }
  *pos += (size_t)n;
  if (*pos >= size) {
    *pos = size - 1;
  }
}

// ---- lock ----------------------------------------------------------------
//
// RAII, because Registry::enable() alone has eight early returns and a
// hand-written take/give pair leaks the mutex the first time someone adds a
// ninth. Also carries the reentrancy flag, so "took the lock" and "marked a
// call in progress" cannot get out of step either.
//
// A null handle (begin() never called) degrades to a no-op rather than
// crashing: a registry that works unlocked is strictly better than a device
// that panics in setup().
class Guard {
 public:
  Guard(SemaphoreHandle_t h, bool *flag) : h_(h), flag_(flag), reentered_(false) {
    if (h_ != nullptr) {
      xSemaphoreTakeRecursive(h_, portMAX_DELAY);
    }
    if (flag_ != nullptr) {
      reentered_ = *flag_;
      if (!reentered_) {
        *flag_ = true;
      }
    }
  }
  ~Guard() {
    if (flag_ != nullptr && !reentered_) {
      *flag_ = false;
    }
    if (h_ != nullptr) {
      xSemaphoreGiveRecursive(h_);
    }
  }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;

  // True when a mutating registry call was ALREADY in progress on this task —
  // i.e. a module callback re-entered the registry that is calling it.
  bool reentered() const { return reentered_; }

 private:
  SemaphoreHandle_t h_;
  bool *flag_;
  bool reentered_;
};

// ---- scheduler trampolines ----------------------------------------------
//
// The Scheduler takes a plain void(*)() with no context, so each module slot
// gets its own one-line thunk. One scheduler entry per module (named after the
// module) keeps the per-module timing visible in the `tasks` command, which is
// how a hogging module gets caught.
template <uint8_t I>
void moduleTick() {
  registry.tickAt(I);
}

SchedulerTaskFn const MODULE_TICKS[] = {
    moduleTick<0>, moduleTick<1>, moduleTick<2>,  moduleTick<3>,  moduleTick<4>,  moduleTick<5>,
    moduleTick<6>, moduleTick<7>, moduleTick<8>,  moduleTick<9>,  moduleTick<10>, moduleTick<11>,
};
static_assert(sizeof(MODULE_TICKS) / sizeof(MODULE_TICKS[0]) == Registry::MAX_MODULES,
              "one tick trampoline per module slot — add/remove thunks when MAX_MODULES changes");

}  // namespace

// ---- error helper --------------------------------------------------------

void cmdErrorf(CmdError *err, const char *code, const char *fmt, ...) {
  if (err == nullptr) {
    return;
  }
  err->code = code;
  va_list args;
  va_start(args, fmt);
  vsnprintf(err->msg, sizeof(err->msg), fmt, args);
  va_end(args);
}

// ---- persisted state, without a Registry ---------------------------------

bool ModulePersist::wasEnabledAtBoot(const char *id) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }

  // NVS IS NOT UP YET when this is called from where it has to be called from.
  // Traced on this toolchain (Arduino-ESP32 3.3.11 / IDF 5.5.5), by
  // disassembling the linked image:
  //
  //   start_cpu0_default()            startup.c:89
  //     do_global_ctors()             <-- static ctors, incl. USBHID/USBMSC and
  //                                       therefore this call. Scheduler NOT
  //                                       running, NVS NOT initialised.
  //   esp_startup_start_app()         app_startup.c:65 -> vTaskStartScheduler()
  //     main_task() -> app_main()
  //       Serial.begin(); USB.begin() <-- tinyusb_init(): DESCRIPTORS FROZEN
  //       initArduino()               <-- nvs_flash_init() finally happens here
  //       xTaskCreate(loopTask) -> setup()
  //
  // So the only hook that exists before the descriptor set is frozen is a
  // static constructor, and at that moment Preferences cannot open anything.
  // Hence the explicit nvs_flash_init() below: it is idempotent, and
  // initArduino()'s own later call then returns ESP_OK immediately.
  //
  // A NO_FREE_PAGES / NEW_VERSION error is deliberately NOT handled here.
  // initArduino() erases and retries in that case; doing it from a static
  // constructor would mean wiping NVS before the app has drawn breath.
  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr != ESP_OK) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return false;  // no namespace yet: nothing has ever been armed
  }
  char buf[Registry::PERSIST_BUF_SIZE];  // same size as the writer's, by construction
  size_t got = prefs.getString(NVS_KEY, buf, sizeof(buf));
  prefs.end();
  if (got == 0) {
    // Absent, empty, or too long for buf — all of which mean "we cannot prove
    // this module was armed". Fail closed: no interface gets bound.
    return false;
  }
  buf[sizeof(buf) - 1] = '\0';

  // ONE parser, shared with restoreFromNvs() (modset.h). The token walk used
  // to be written out twice, in two files, for one on-flash format.
  return ModSet::contains(buf, id);
}

// ---- registration --------------------------------------------------------

void Registry::begin() {
  if (lock_ == nullptr) {
    lock_ = xSemaphoreCreateRecursiveMutex();
  }
}

bool Registry::add(const ModuleDescriptor *desc) {
  Guard g(lock_, nullptr);

  if (sealed_) {
    // mods_[] is only safe to hand out as raw pointers, and enabled_/desired_
    // only safe to index, because nothing registers after boot. Enforced, not
    // assumed.
    return false;
  }
  if (count_ >= MAX_MODULES || desc == nullptr || desc->id == nullptr || desc->id[0] == '\0') {
    return false;
  }
  if (strlen(desc->id) > MAX_ID_LEN) {
    return false;  // would overrun the persisted set; see PERSIST_BUF_SIZE
  }
  if (indexOf(desc->id) >= 0) {
    return false;  // duplicate id: ids are wire-visible and must be unique
  }

  // Register the tick BEFORE committing the module, so a full scheduler table
  // fails the whole registration instead of leaving a module whose periodic
  // work silently never runs.
  if (desc->tick != nullptr && !scheduler.addTask(desc->id, desc->tickIntervalMs, MODULE_TICKS[count_])) {
    return false;
  }

  mods_[count_] = desc;
  enabled_[count_] = false;
  desired_[count_] = false;
  count_++;
  return true;
}

int8_t Registry::indexOf(const char *id) const {
  if (id == nullptr) {
    return -1;
  }
  for (uint8_t i = 0; i < count_; i++) {
    if (strcmp(mods_[i]->id, id) == 0) {
      return (int8_t)i;
    }
  }
  return -1;
}

bool Registry::isEnabled(const char *id) {
  Guard g(lock_, nullptr);
  int8_t idx = indexOf(id);
  return idx >= 0 && enabled_[idx];
}

void Registry::tickAt(uint8_t i) {
  Guard g(lock_, nullptr);
  if (i >= count_ || !enabled_[i] || mods_[i]->tick == nullptr) {
    return;  // disabled (or gone): the module's periodic work stops dead
  }
  mods_[i]->tick();
}

// ---- start/stop ----------------------------------------------------------
//
// A module's claims are held if and only if enabled_[i] is true. There is no
// separate claim table, and that is the point: releasing a claim and clearing
// the enabled flag are the same single operation, so a claim cannot be leaked
// by any error path below.

bool Registry::startModule(uint8_t idx, const char **errMsg) {
  *errMsg = nullptr;

  // Take the claims *before* calling enable(), so the module is on the record
  // as holding them for the whole of its own start-up. If enable() fails, the
  // flag goes back to false and every claim is released with it.
  enabled_[idx] = true;
  if (mods_[idx]->enable != nullptr && !mods_[idx]->enable(errMsg)) {
    enabled_[idx] = false;
    if (!restoring_) {
      desired_[idx] = false;
    }
    return false;
  }
  if (!restoring_) {
    desired_[idx] = true;
  }
  return true;
}

bool Registry::stopModule(uint8_t idx, const char **errMsg) {
  *errMsg = nullptr;

  bool ok = true;
  if (mods_[idx]->disable != nullptr) {
    ok = mods_[idx]->disable(errMsg);
  }

  // STANCE ON A FAILED disable(): release the claims anyway, and report the
  // failure to the caller. The alternative — leave the module marked enabled —
  // wedges the registry permanently: the resource stays claimed, nothing else
  // can ever take it, and the only way out is a reboot. A module that cannot
  // tear itself down is a bug in that module; refusing to reuse its resources
  // forever does not fix that bug, it just spreads the damage. So the state is
  // always "disabled" afterwards, and the error says so explicitly.
  enabled_[idx] = false;
  if (!restoring_) {
    desired_[idx] = false;
  }
  return ok;
}

// ---- arbitration ---------------------------------------------------------

uint8_t Registry::blockersOf(uint8_t idx, const char **ids, uint8_t maxIds, char *why, size_t whySize) const {
  uint8_t n = 0;
  size_t pos = 0;
  if (why != nullptr && whySize > 0) {
    why[0] = '\0';
  }

  // Collect EVERY blocker, not just the first: a caller told "blocked by hid"
  // that then disables hid and is told "blocked by storage" has been made to
  // guess, twice.
  for (uint8_t j = 0; j < count_; j++) {
    if (j == idx || !enabled_[j]) {
      continue;
    }
    uint8_t res = Claims::firstConflict(mods_[idx]->claims, mods_[j]->claims);
    if (res == Claims::RES_COUNT) {
      continue;
    }
    if (ids != nullptr && n < maxIds) {
      ids[n] = mods_[j]->id;
    }
    n++;
    if (why != nullptr) {
      appendf(why, whySize, &pos, "%s'%s' holds %s (%s)", pos ? "; " : "", mods_[j]->id, Claims::resourceName(res),
              Claims::modeName(mods_[j]->claims.mode[res]));
    }
  }
  return n < maxIds ? n : maxIds;
}

// ---- enable / disable ----------------------------------------------------

void Registry::enable(const char *id, bool force, ModuleActionResult &out) {
  out = ModuleActionResult{};

  Guard g(lock_, &inCall_);
  if (g.reentered()) {
    out.code = "EREENTRANT";
    snprintf(out.msg, sizeof(out.msg),
             "refusing to enable '%s': a module callback re-entered the registry mid-operation", id ? id : "");
    return;
  }

  int8_t idxSigned = indexOf(id);
  if (idxSigned < 0) {
    out.code = "ENOMOD";
    snprintf(out.msg, sizeof(out.msg), "no such module: '%s'", id ? id : "");
    return;
  }
  uint8_t idx = (uint8_t)idxSigned;
  const ModuleDescriptor *desc = mods_[idx];

  // Already enabled AND already armed: a no-op success, not an error.
  // Idempotence matters here because a UI, a persisted-state replay and a
  // script can all ask at once.
  if (enabled_[idx] && desired_[idx]) {
    out.ok = true;
    out.changed = false;
    out.enabledAfter = true;
    snprintf(out.msg, sizeof(out.msg), "module '%s' is already enabled", desc->id);
    return;
  }

  // Boot-time-bound module, live request: its USB interface was accepted or
  // refused before setup() ran and nothing here can change that now. Record
  // the intent, touch no hardware, and say plainly that it needs a reboot.
  // (During restoreFromNvs() this branch is skipped — the boot binding has
  // just happened, so the normal path runs and the module's own enable()
  // verifies the interface really is there.)
  //
  // NOTE: claims are deliberately NOT evaluated here. An armed-but-unbound
  // module holds nothing, so there is nothing to arbitrate yet; the conflict
  // check happens at the next boot, in restoreFromNvs(), against whatever is
  // actually running then. Arming cannot be refused for a conflict that may
  // well be gone by the time it matters.
  if (desc->bootTimeBinding && !restoring_) {
    bool wasArmed = desired_[idx];
    desired_[idx] = true;
    persist();
    out.ok = true;
    out.changed = !wasArmed;
    out.enabledAfter = enabled_[idx];
    out.pendingRestart = !enabled_[idx];
    if (out.pendingRestart) {
      out.code = "EREBOOT";
      snprintf(out.msg, sizeof(out.msg),
               "module '%s' is armed%s and will bind its USB interface at the next boot; it is NOT running now. "
               "Reboot to apply.",
               desc->id, wasArmed ? " (already)" : "");
    } else {
      snprintf(out.msg, sizeof(out.msg), "module '%s' is already bound and running", desc->id);
    }
    return;
  }

  char why[160];
  out.blockedByCount = blockersOf(idx, out.blockedBy, ModuleActionResult::MAX_LIST, why, sizeof(why));

  if (out.blockedByCount > 0) {
    if (!force) {
      // Default policy: refuse. Nothing is ever stopped without an explicit
      // p:{"force":true} from the caller.
      out.ok = false;
      out.code = "EBUSY";
      out.enabledAfter = false;
      // appendf, not snprintf: `why` can be up to sizeof(why) on its own, so
      // a single snprintf of prefix + why + advice is a provable truncation
      // (GCC -Wformat-truncation catches it). Building it in pieces truncates
      // the *tail* gracefully instead, and the blocker ids are also in
      // out.blockedBy[] regardless.
      size_t mp = 0;
      out.msg[0] = '\0';
      appendf(out.msg, sizeof(out.msg), &mp, "cannot enable '%s': %s", desc->id, why);
      appendf(out.msg, sizeof(out.msg), &mp, ". Retry with p:{\"force\":true} to stop them.");
      return;
    }

    // An `essential` blocker is not stoppable by anyone, force included —
    // `cdc` is the link this very reply is going out on. Checked BEFORE
    // anything is stopped, so a refusal costs no collateral.
    for (uint8_t j = 0; j < count_; j++) {
      if (j == idx || !enabled_[j] || !mods_[j]->essential ||
          Claims::coexist(desc->claims, mods_[j]->claims)) {
        continue;
      }
      out.ok = false;
      out.code = "EESSENTIAL";
      out.enabledAfter = false;
      snprintf(out.msg, sizeof(out.msg),
               "cannot enable '%s' even with force: '%s' blocks it and cannot be stopped (%s). Nothing was changed.",
               desc->id, mods_[j]->id, mods_[j]->name);
      return;
    }

    for (uint8_t j = 0; j < count_; j++) {
      if (j == idx || !enabled_[j] || Claims::coexist(desc->claims, mods_[j]->claims)) {
        continue;
      }
      const char *derr = nullptr;
      if (stopModule(j, &derr)) {
        if (out.stoppedCount < ModuleActionResult::MAX_LIST) {
          out.stopped[out.stoppedCount++] = mods_[j]->id;
        }
        continue;
      }

      // Abort rather than press on: the blocker's claims were released (see
      // stopModule) but its hardware state is now unknown, and starting the
      // target on top of that is how you get a wedged bus. Say exactly what
      // was stopped, what refused to stop, and that the target did NOT start.
      if (out.failedToStopCount < ModuleActionResult::MAX_LIST) {
        out.failedToStop[out.failedToStopCount++] = mods_[j]->id;
      }
      out.ok = false;
      out.code = "EDISABLE";
      out.enabledAfter = false;
      persist();
      snprintf(out.msg, sizeof(out.msg),
               "force-enable of '%s' aborted: '%s' failed to stop (%s). %u module(s) were stopped cleanly; '%s' is "
               "NOT enabled.",
               desc->id, mods_[j]->id, derr ? derr : "no reason given", (unsigned)out.stoppedCount, desc->id);
      return;
    }
  }

  const char *eerr = nullptr;
  if (!startModule(idx, &eerr)) {
    out.ok = false;
    out.code = "EENABLE";
    out.enabledAfter = false;
    persist();  // the forced stops above are real state; record them
    if (out.stoppedCount > 0) {
      snprintf(out.msg, sizeof(out.msg),
               "module '%s' failed to start (%s) AFTER stopping %u module(s); '%s' is disabled and its claims are "
               "released, the stopped modules are still stopped.",
               desc->id, eerr ? eerr : "no reason given", (unsigned)out.stoppedCount, desc->id);
    } else {
      snprintf(out.msg, sizeof(out.msg), "module '%s' failed to start (%s); its claims were released.", desc->id,
               eerr ? eerr : "no reason given");
    }
    return;
  }

  out.ok = true;
  out.changed = true;
  out.enabledAfter = true;
  persist();
  if (out.stoppedCount > 0) {
    snprintf(out.msg, sizeof(out.msg), "module '%s' enabled; %u conflicting module(s) were stopped to do it.",
             desc->id, (unsigned)out.stoppedCount);
  } else {
    snprintf(out.msg, sizeof(out.msg), "module '%s' enabled", desc->id);
  }
}

void Registry::disable(const char *id, ModuleActionResult &out) {
  out = ModuleActionResult{};

  Guard g(lock_, &inCall_);
  if (g.reentered()) {
    out.code = "EREENTRANT";
    snprintf(out.msg, sizeof(out.msg),
             "refusing to disable '%s': a module callback re-entered the registry mid-operation", id ? id : "");
    return;
  }

  int8_t idxSigned = indexOf(id);
  if (idxSigned < 0) {
    out.code = "ENOMOD";
    snprintf(out.msg, sizeof(out.msg), "no such module: '%s'", id ? id : "");
    return;
  }
  uint8_t idx = (uint8_t)idxSigned;
  const ModuleDescriptor *desc = mods_[idx];

  if (desc->essential) {
    // Refused in the registry, not in the module's disable(): stopModule()
    // clears enabled_ even when disable() returns false (see the stance
    // above), so a module cannot veto its own shutdown from inside the
    // callback. The veto has to live here to mean anything.
    out.ok = false;
    out.code = "EESSENTIAL";
    out.enabledAfter = enabled_[idx];
    snprintf(out.msg, sizeof(out.msg), "module '%s' (%s) cannot be disabled: it is this device's control link.",
             desc->id, desc->name);
    return;
  }

  if (desc->bootTimeBinding && !restoring_) {
    // Symmetric with enable(): a bound USB interface cannot be withdrawn at
    // runtime. Disarm it for the next boot and say so; do NOT claim it stopped.
    bool wasArmed = desired_[idx];
    desired_[idx] = false;
    persist();
    out.ok = true;
    out.changed = wasArmed;
    out.enabledAfter = enabled_[idx];
    out.pendingRestart = enabled_[idx];
    if (out.pendingRestart) {
      out.code = "EREBOOT";
      snprintf(out.msg, sizeof(out.msg),
               "module '%s' is disarmed and will not bind at the next boot, but its USB interface is already bound "
               "and STAYS ACTIVE until you reboot.",
               desc->id);
    } else {
      snprintf(out.msg, sizeof(out.msg), "module '%s' is disarmed; it was not bound at this boot.", desc->id);
    }
    return;
  }

  if (!enabled_[idx]) {
    // Not live — so its disable() must NOT be called. This is not only the
    // "already off" case: a module that is persisted-on but FAILED to start at
    // boot (or was blocked by a conflict) sits here with desired_ still set,
    // and calling a teardown on hardware that was never brought up is how you
    // get a driver deinitialising a peripheral it does not own. All this can
    // legitimately do is withdraw the intent.
    bool wasDesired = desired_[idx];
    desired_[idx] = false;
    persist();
    out.ok = true;
    out.changed = wasDesired;
    out.enabledAfter = false;
    if (wasDesired) {
      snprintf(out.msg, sizeof(out.msg),
               "module '%s' was not running (blocked or failed to start at boot); removed from the persisted set so "
               "it will not be retried.",
               desc->id);
    } else {
      snprintf(out.msg, sizeof(out.msg), "module '%s' is already disabled", desc->id);
    }
    return;
  }

  const char *derr = nullptr;
  bool ok = stopModule(idx, &derr);
  out.changed = true;
  out.enabledAfter = false;
  persist();

  if (!ok) {
    out.ok = false;
    out.code = "EDISABLE";
    if (out.failedToStopCount < ModuleActionResult::MAX_LIST) {
      out.failedToStop[out.failedToStopCount++] = desc->id;
    }
    snprintf(out.msg, sizeof(out.msg),
             "module '%s' reported a failed shutdown (%s); its claims were released anyway and it is now disabled.",
             desc->id, derr ? derr : "no reason given");
    return;
  }

  out.ok = true;
  if (out.stoppedCount < ModuleActionResult::MAX_LIST) {
    out.stopped[out.stoppedCount++] = desc->id;
  }
  snprintf(out.msg, sizeof(out.msg), "module '%s' disabled", desc->id);
}

// ---- listing / dispatch --------------------------------------------------

void Registry::list(JsonArray out) {
  Guard g(lock_, nullptr);

  const char *blockers[MAX_MODULES];

  for (uint8_t i = 0; i < count_; i++) {
    const ModuleDescriptor *m = mods_[i];
    JsonObject o = out.add<JsonObject>();
    o["id"] = m->id;
    o["name"] = m->name;
    o["category"] = m->category;
    o["enabled"] = enabled_[i];
    o["essential"] = m->essential;
    o["boot_time_binding"] = m->bootTimeBinding;
    // Everything a UI needs to render "reboot to apply" without knowing why:
    // `armed` is the persisted intent, `pending_restart` is intent != live.
    o["armed"] = desired_[i];
    o["pending_restart"] = desired_[i] != enabled_[i];

    // Only non-NONE claims are rendered, so the object reads as "what this
    // module takes" rather than a wall of "none".
    JsonObject claims = o["claims"].to<JsonObject>();
    for (uint8_t r = 0; r < Claims::RES_COUNT; r++) {
      if (m->claims.mode[r] != Claims::CLAIM_NONE) {
        claims[Claims::resourceName(r)] = Claims::modeName(m->claims.mode[r]);
      }
    }

    // Same firstConflict() walk enable() uses — see blockersOf(). Empty for an
    // already-enabled module, by definition. This is here so the arbitration
    // rule is not reimplemented in JavaScript and then allowed to drift from
    // the one the device actually enforces.
    uint8_t nb = blockersOf(i, blockers, MAX_MODULES, nullptr, 0);
    JsonArray blockedBy = o["blocked_by"].to<JsonArray>();
    for (uint8_t b = 0; b < nb; b++) {
      blockedBy.add(blockers[b]);
    }

    // What the module DOES, and what each action TAKES. Static .rodata, so
    // this costs flash, not RAM.
    //
    // `params` is an ARRAY OF OBJECTS, not a prose string:
    //   {"name":"rgb","type":"string","required":true,"help":"..."}
    //   {"name":"wpm","type":"int","required":false,"help":"...","min":1,"max":2000}
    //   {"name":"name","type":"enum","required":true,"help":"...","enum":["status","diag"]}
    // Keys are omitted when they do not apply — an absent "min" means the
    // dispatch imposes no lower bound, which is not the same as INT32_MIN.
    // This is the shape W4's real UI consumes too, so keep it obvious.
    JsonArray actions = o["actions"].to<JsonArray>();
    for (uint8_t a = 0; a < m->actionCount; a++) {
      const ModuleAction &act = m->actions[a];
      JsonObject ao = actions.add<JsonObject>();
      ao["act"] = act.act;
      ao["help"] = act.help;
      JsonArray params = ao["params"].to<JsonArray>();
      uint8_t nParams = (act.params != nullptr) ? act.paramCount : 0;
      for (uint8_t q = 0; q < nParams; q++) {
        const ModuleParam &pd = act.params[q];
        JsonObject po = params.add<JsonObject>();
        po["name"] = pd.name;
        po["type"] = ModParam::typeName(pd.type);
        po["required"] = pd.required;
        if (pd.help != nullptr && pd.help[0] != '\0') {
          po["help"] = pd.help;
        }
        if (pd.type == P_ENUM || pd.type == P_ENUM_LIST) {
          JsonArray vals = po["enum"].to<JsonArray>();
          char v[ModParam::MAX_ENUM_VALUE + 1];
          uint8_t n = ModParam::enumCount(pd.enumVals);
          bool truncated = false;
          for (uint8_t e = 0; e < n; e++) {
            // The RETURN is the value's full length, so an over-long value is
            // reported rather than silently shortened into a value the module
            // would then reject.
            if (ModParam::enumValueAt(pd.enumVals, e, v, sizeof(v)) >= sizeof(v)) {
              truncated = true;
            }
            // Cast for the same reason mod_storage.cpp casts b64Buf/hex: `v`
            // is a stack buffer, and this makes the "ArduinoJson copies it"
            // expectation explicit rather than dependent on constness.
            vals.add((const char *)v);
          }
          if (truncated) {
            po["enum_truncated"] = true;
          }
        }
        if (pd.type == P_INT) {
          if (pd.min != ModParam::NO_MIN) {
            po["min"] = pd.min;
          }
          if (pd.max != ModParam::NO_MAX) {
            po["max"] = pd.max;
          }
        }
      }
    }

    // status() is only called while the module is enabled — a disabled module
    // may have deinitialised the very peripheral it would read. The lock is
    // held across the call, so that is now enforced rather than hoped for.
    if (enabled_[i] && m->status != nullptr) {
      JsonObject s = o["status"].to<JsonObject>();
      m->status(s);
    }
  }
}

bool Registry::statusOf(const char *id, JsonObject out) {
  // Same Guard shape as list(): read-only, so no reentrancy flag. That is what
  // makes this safe to call from inside a module's own tick, which already
  // holds the (recursive) lock via tickAt().
  Guard g(lock_, nullptr);

  int8_t idx = indexOf(id);
  if (idx < 0 || !enabled_[idx] || mods_[idx]->status == nullptr) {
    return false;
  }
  mods_[idx]->status(out);
  return true;
}

DispatchResult Registry::dispatch(const char *id, const char *act, const CmdContext &ctx, JsonObjectConst p,
                                  JsonObject d, CmdError *err) {
  Guard g(lock_, &inCall_);
  if (g.reentered()) {
    cmdErrorf(err, "EREENTRANT", "module '%s' re-entered the registry from a callback", id ? id : "");
    return DISPATCH_FAIL;
  }

  int8_t idxSigned = indexOf(id);
  if (idxSigned < 0) {
    cmdErrorf(err, "ENOMOD", "no such module: '%s'", id ? id : "");
    return DISPATCH_FAIL;
  }
  uint8_t idx = (uint8_t)idxSigned;
  const ModuleDescriptor *m = mods_[idx];

  if (!enabled_[idx]) {
    if (m->bootTimeBinding && desired_[idx]) {
      // Armed but not bound. "enable it first" would be a lie — it IS enabled,
      // as far as the user's last instruction goes; what it needs is a reboot.
      cmdErrorf(err, "EREBOOT", "module '%s' is armed but only binds at boot; reboot to use it", m->id);
      return DISPATCH_FAIL;
    }
    cmdErrorf(err, "EDISABLED", "module '%s' is disabled; enable it first", m->id);
    return DISPATCH_FAIL;
  }

  if (m->dispatch == nullptr) {
    cmdErrorf(err, "ENOACT", "module '%s' takes no actions", m->id);
    return DISPATCH_FAIL;
  }

  if (act == nullptr) {
    cmdErrorf(err, "ENOACT", "missing \"act\" for module '%s'", m->id);
    return DISPATCH_FAIL;
  }

  return m->dispatch(ctx, act, p, d, err);
}

// ---- persistence ---------------------------------------------------------

void Registry::persist() {
  if (restoring_) {
    // Replaying stored intent is not changing it. A module skipped this boot
    // therefore stays in the persisted set and comes back on its own once the
    // conflict is gone.
    return;
  }

  // desired_, NOT enabled_: they differ for a bootTimeBinding module that has
  // been armed or disarmed since the last reboot, and for a module that was
  // skipped at boot because its resources were taken. Persisting enabled_
  // would quietly delete both kinds of intent at the next write.
  char out[PERSIST_BUF_SIZE];
  out[0] = '\0';
  bool fits = true;
  for (uint8_t i = 0; i < count_; i++) {
    if (desired_[i] && !ModSet::append(out, sizeof(out), mods_[i]->id)) {
      // Cannot happen while PERSIST_BUF_SIZE covers MAX_MODULES x MAX_ID_LEN
      // (static_assert in the header) — but a silent truncation here means
      // modules stop coming back after a reboot with nothing saying why, so it
      // is reported rather than trusted to arithmetic.
      fits = false;
    }
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    report_.nvsWriteOk = false;
    return;
  }
  size_t pos = strlen(out);
  size_t written = prefs.putString(NVS_KEY, out);
  prefs.end();

  // putString() returns strlen(value), so an empty set legitimately returns 0
  // and cannot be distinguished from a failure that way. Only the non-empty
  // case is checked.
  report_.nvsWriteOk = fits && ((pos == 0) || (written == pos));
}

// Writes the set of ids this firmware registers, so the NEXT boot can tell
// "absent because new" from "absent because the owner turned it off"
// (modset.h). Called once, from restoreFromNvs(), and ONLY when the stored
// value actually differs — the caller has already read it, and the set changes
// when the firmware changes rather than when a module is toggled, so this is
// not a per-boot flash write.
void Registry::persistKnown(const char *current) {
  Preferences w;
  if (!w.begin(NVS_NAMESPACE, false)) {
    report_.nvsWriteOk = false;
    return;
  }
  size_t want = strlen(current);
  size_t written = w.putString(NVS_KEY_KNOWN, current);
  w.end();
  if (want != 0 && written != want) {
    report_.nvsWriteOk = false;
  }
}

void Registry::restoreFromNvs() {
  Guard g(lock_, nullptr);

  report_ = ModuleRestoreReport{};
  report_.nvsWriteOk = true;
  memset(buf_, 0, sizeof(buf_));

  size_t storedLen = 0;  // NVS length INCLUDING the NUL, 0 if the key is absent
  size_t got = 0;
  // The KNOWN set. A stack buffer, not a member: nothing in report_ points
  // into it (unlike buf_, which report_.unknown indexes), so it costs no
  // static RAM at all — 192 bytes of the loop task's 8 K stack, in setup().
  char known[PERSIST_BUF_SIZE];
  known[0] = '\0';
  size_t knownStoredLen = 0;
  size_t knownGot = 0;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    storedLen = prefs.getStringLength(NVS_KEY);
    got = prefs.getString(NVS_KEY, buf_, sizeof(buf_));
    knownStoredLen = prefs.getStringLength(NVS_KEY_KNOWN);
    knownGot = prefs.getString(NVS_KEY_KNOWN, known, sizeof(known));
    prefs.end();
  }
  buf_[sizeof(buf_) - 1] = '\0';
  known[sizeof(known) - 1] = '\0';
  // Same three-way read as the enabled set below. An over-long stored value
  // (getString returns 0 and leaves the buffer untouched) is treated as
  // ABSENT, i.e. conservatively: nothing gets defaulted back on because a
  // string would not fit a buffer.
  report_.knownRead = (knownStoredLen > 0 && knownGot > 0);
  if (!report_.knownRead) {
    known[0] = '\0';
  }

  // Three distinct outcomes, and the old code collapsed them into one.
  //   storedLen == 0            -> nothing persisted: first boot, or NVS erased.
  //   storedLen > 0, got > 0    -> read it (note "" is a legitimate stored value:
  //                                the user turned everything off).
  //   storedLen > 0, got == 0   -> the stored string is LONGER than buf_.
  //     Preferences::getString() returns 0 and leaves the buffer untouched, so
  //     this used to look exactly like "nothing is enabled": every module
  //     silently off, no error anywhere, forever. Now it is reportable.
  report_.nvsRead = storedLen > 0;
  report_.nvsStoredLen = storedLen;
  report_.nvsTooLong = (storedLen > 0 && got == 0);
  if (report_.nvsTooLong) {
    buf_[0] = '\0';  // do not parse a buffer getString() never wrote to
  }

  bool want[MAX_MODULES] = {false};

  // ---- PASS 1: the decision, against the lists while they are still INTACT.
  //
  // ModSet::wantAtBoot() is the whole rule and it is host-tested (modset.h,
  // test_modset). nullptr for the enabled list means "the key is absent", i.e.
  // a virgin device, and every module takes its own default; nullptr for the
  // known list means this device predates the known set, and nothing is
  // defaulted back on. Order matters: pass 2 destroys buf_.
  const char *enabledList = report_.nvsRead ? buf_ : nullptr;
  const char *knownList = report_.knownRead ? known : nullptr;
  for (uint8_t i = 0; i < count_; i++) {
    want[i] = ModSet::wantAtBoot(enabledList, knownList, mods_[i]->id, mods_[i]->defaultEnabled);
    // Turned on because this device has never heard of it. Recorded so that
    // "why is that suddenly on?" is answerable from the `modules` command
    // rather than from the source.
    if (want[i] && enabledList != nullptr && !ModSet::contains(enabledList, mods_[i]->id) &&
        report_.defaultedCount < ModuleRestoreReport::MAX_LIST) {
      report_.defaulted[report_.defaultedCount++] = mods_[i]->id;
    }
  }

  // ---- PASS 2: persisted ids this firmware no longer builds.
  //
  // Split in place. Each token stays a NUL-terminated string inside buf_,
  // which is a member, so report_.unknown[] may safely point into it — and
  // buf_ is unusable as a list from here on, which is why the decision above
  // had to come first.
  if (report_.nvsRead && !report_.nvsTooLong) {
    char *cur = buf_;
    while (cur != nullptr && *cur != '\0') {
      char *comma = strchr(cur, ',');
      if (comma != nullptr) {
        *comma = '\0';
      }
      if (*cur != '\0' && indexOf(cur) < 0 && report_.unknownCount < ModuleRestoreReport::MAX_LIST) {
        // A persisted id we no longer build. Recorded, then ignored — never
        // fatal, and it does not stop the rest of the set being restored.
        report_.unknown[report_.unknownCount++] = cur;
      }
      cur = (comma != nullptr) ? comma + 1 : nullptr;
    }
  }

  // An essential module comes up whatever NVS says — including when NVS is
  // unreadable, which is precisely when you most want the console.
  for (uint8_t i = 0; i < count_; i++) {
    if (mods_[i]->essential) {
      want[i] = true;
    }
  }

  // Record the intent BEFORE trying to act on it, and leave it alone
  // afterwards: a module that fails to start or is skipped this boot keeps its
  // place in the persisted set, so the next persist() (triggered by some
  // unrelated toggle) cannot quietly delete it.
  for (uint8_t i = 0; i < count_; i++) {
    desired_[i] = want[i];
  }

  // Replay in REGISTRATION order, not NVS order, so the outcome of an
  // unsatisfiable set is deterministic instead of depending on how the string
  // happened to be written.
  restoring_ = true;
  for (uint8_t i = 0; i < count_; i++) {
    if (!want[i]) {
      continue;
    }
    if (mods_[i]->bootTimeBinding && report_.armedCount < ModuleRestoreReport::MAX_LIST) {
      // Whether it actually bound is decided by the module's own file-scope
      // check against ModulePersist::wasEnabledAtBoot(); its enable() below
      // verifies that and fails loudly if the interface is missing.
      report_.armed[report_.armedCount++] = mods_[i]->id;
    }
    ModuleActionResult r;
    enable(mods_[i]->id, false, r);
    if (r.ok && enabled_[i]) {
      if (report_.restoredCount < ModuleRestoreReport::MAX_LIST) {
        report_.restored[report_.restoredCount++] = mods_[i]->id;
      }
    } else if (report_.skippedCount < ModuleRestoreReport::MAX_LIST) {
      // Conflicting or failed to start. Skipped, recorded, keep booting.
      report_.skipped[report_.skippedCount++] = mods_[i]->id;
    }
  }
  restoring_ = false;

  // ---- record what this firmware knows about, for the NEXT boot ----------
  //
  // Written AFTER the replay, and from the REGISTERED set rather than from
  // what actually started: a module that was blocked or failed to start is
  // still a module this device has heard of, and re-defaulting it every boot
  // would be a loop that quietly overrode the owner.
  char knownNow[PERSIST_BUF_SIZE];
  knownNow[0] = '\0';
  for (uint8_t i = 0; i < count_; i++) {
    if (!ModSet::append(knownNow, sizeof(knownNow), mods_[i]->id)) {
      // Unreachable while the PERSIST_BUF_SIZE static_assert holds. If it ever
      // is reached, the known set is short — which would re-default the missing
      // modules next boot — so it is reported rather than written silently.
      report_.nvsWriteOk = false;
    }
  }
  // A module that was defaulted ON has just changed the intent, and persist()
  // was suppressed for the whole replay (restoring_). Without this write the
  // default would be applied again on EVERY boot: the id would be in the known
  // set but still absent from the enabled set, so the next boot would decide
  // "off" and the module would flap on for one boot and off the next.
  //
  // NOT written when the stored set was too long to read: that value is intact
  // on flash and recoverable, and overwriting it with what we could not read
  // would destroy the owner's configuration to fix a display bug.
  //
  // BEFORE the known set, deliberately. If power is lost between the two
  // writes, the enabled set is the one that has to have landed: the new
  // default is then honoured next boot regardless. The other order loses it.
  // (It is also why this is not folded into persist(), which sets nvsWriteOk
  // absolutely and would clear a failure reported by persistKnown().)
  if (report_.defaultedCount > 0 && !report_.nvsTooLong) {
    persist();
  }

  // Only when it moved: `known` is what this boot read, and rewriting an
  // identical string every boot would be a flash write per power cycle for
  // nothing.
  if (!report_.knownRead || strcmp(known, knownNow) != 0) {
    persistKnown(knownNow);
  }

  // From here on the module set is fixed. add() fails.
  sealed_ = true;
}
