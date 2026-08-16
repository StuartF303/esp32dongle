#include "registry.h"

#include <Preferences.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

Registry registry;

namespace {

// NVS storage for the enabled-module set. One string key, "id1,id2,...",
// rather than a bitmask over registration indices: a bitmask silently means
// something different the moment modules are reordered or one is removed,
// which is exactly the "code changed" case this has to survive.
//
// This lives in the 32 K `nvs` partition at 0x9000 (ARCHITECTURE.md section 3;
// the 4 K `nvs_keys` at 0x11000 is reserved-but-unused, for NVS encryption
// later). That partition also has to carry Wi-Fi STA config, an auth PIN and a
// rotating session token, and NVS needs spare pages for compaction — so one
// short string here, not a key per module.
const char *NVS_NAMESPACE = "modreg";
const char *NVS_KEY = "on";

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

}  // namespace

// ---- registration --------------------------------------------------------

bool Registry::add(const ModuleDescriptor *desc) {
  if (count_ >= MAX_MODULES || desc == nullptr || desc->id == nullptr || desc->id[0] == '\0') {
    return false;
  }
  if (indexOf(desc->id) >= 0) {
    return false;  // duplicate id: ids are wire-visible and must be unique
  }
  mods_[count_] = desc;
  enabled_[count_] = false;
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

bool Registry::isEnabled(const char *id) const {
  int8_t idx = indexOf(id);
  return idx >= 0 && enabled_[idx];
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
    return false;
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
  return ok;
}

// ---- enable / disable ----------------------------------------------------

void Registry::enable(const char *id, bool force, ModuleActionResult &out) {
  out = ModuleActionResult{};

  int8_t idxSigned = indexOf(id);
  if (idxSigned < 0) {
    out.code = "ENOMOD";
    snprintf(out.msg, sizeof(out.msg), "no such module: '%s'", id ? id : "");
    return;
  }
  uint8_t idx = (uint8_t)idxSigned;
  const ModuleDescriptor *desc = mods_[idx];

  // Already enabled: a no-op success, not an error. Idempotence matters here
  // because a UI, a persisted-state replay and a script can all ask at once.
  if (enabled_[idx]) {
    out.ok = true;
    out.changed = false;
    out.enabledAfter = true;
    snprintf(out.msg, sizeof(out.msg), "module '%s' is already enabled", desc->id);
    return;
  }

  // Collect EVERY blocker, not just the first: a caller told "blocked by hid"
  // that then disables hid and is told "blocked by storage" has been made to
  // guess, twice.
  size_t pos = 0;
  char why[160];
  why[0] = '\0';
  for (uint8_t j = 0; j < count_; j++) {
    if (j == idx || !enabled_[j]) {
      continue;
    }
    uint8_t res = Claims::firstConflict(desc->claims, mods_[j]->claims);
    if (res == Claims::RES_COUNT) {
      continue;
    }
    if (out.blockedByCount < ModuleActionResult::MAX_LIST) {
      out.blockedBy[out.blockedByCount++] = mods_[j]->id;
    }
    appendf(why, sizeof(why), &pos, "%s'%s' holds %s (%s)", pos ? "; " : "", mods_[j]->id,
            Claims::resourceName(res), Claims::modeName(mods_[j]->claims.mode[res]));
  }

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

    for (uint8_t j = 0; j < count_; j++) {
      if (j == idx || !enabled_[j] || Claims::coexist(desc->claims, mods_[j]->claims)) {
        continue;
      }
      const char *derr = nullptr;
      bool stopped = stopModule(j, &derr);
      if (out.stoppedCount < ModuleActionResult::MAX_LIST) {
        out.stopped[out.stoppedCount++] = mods_[j]->id;
      }
      if (!stopped) {
        // Abort rather than press on: the blocker's claims were released (see
        // stopModule) but its hardware state is now unknown, and starting the
        // target on top of that is how you get a wedged bus. Say exactly what
        // was stopped and that the target did NOT start.
        out.ok = false;
        out.code = "EDISABLE";
        out.enabledAfter = false;
        persist();
        snprintf(out.msg, sizeof(out.msg),
                 "force-enable of '%s' aborted: '%s' failed to stop (%s). %u module(s) were stopped; '%s' is NOT "
                 "enabled.",
                 desc->id, mods_[j]->id, derr ? derr : "no reason given", (unsigned)out.stoppedCount, desc->id);
        return;
      }
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

  int8_t idxSigned = indexOf(id);
  if (idxSigned < 0) {
    out.code = "ENOMOD";
    snprintf(out.msg, sizeof(out.msg), "no such module: '%s'", id ? id : "");
    return;
  }
  uint8_t idx = (uint8_t)idxSigned;
  const ModuleDescriptor *desc = mods_[idx];

  if (!enabled_[idx]) {
    out.ok = true;
    out.changed = false;
    out.enabledAfter = false;
    snprintf(out.msg, sizeof(out.msg), "module '%s' is already disabled", desc->id);
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
    snprintf(out.msg, sizeof(out.msg),
             "module '%s' reported a failed shutdown (%s); its claims were released anyway and it is now disabled.",
             desc->id, derr ? derr : "no reason given");
    return;
  }

  out.ok = true;
  snprintf(out.msg, sizeof(out.msg), "module '%s' disabled", desc->id);
}

// ---- listing / dispatch --------------------------------------------------

void Registry::list(JsonArray out) const {
  for (uint8_t i = 0; i < count_; i++) {
    const ModuleDescriptor *m = mods_[i];
    JsonObject o = out.add<JsonObject>();
    o["id"] = m->id;
    o["name"] = m->name;
    o["category"] = m->category;
    o["enabled"] = enabled_[i];

    // Only non-NONE claims are rendered, so the object reads as "what this
    // module takes" rather than a wall of "none".
    JsonObject claims = o["claims"].to<JsonObject>();
    for (uint8_t r = 0; r < Claims::RES_COUNT; r++) {
      if (m->claims.mode[r] != Claims::CLAIM_NONE) {
        claims[Claims::resourceName(r)] = Claims::modeName(m->claims.mode[r]);
      }
    }

    // status() is only called while the module is enabled — a disabled module
    // may have deinitialised the very peripheral it would read.
    if (enabled_[i] && m->status != nullptr) {
      JsonObject s = o["status"].to<JsonObject>();
      m->status(s);
    }
  }
}

bool Registry::dispatch(const char *id, const char *act, JsonObjectConst p, JsonObject d, const char **errCode,
                        const char **errMsg) {
  int8_t idxSigned = indexOf(id);
  if (idxSigned < 0) {
    *errCode = "ENOMOD";
    snprintf(errBuf_, sizeof(errBuf_), "no such module: '%s'", id ? id : "");
    *errMsg = errBuf_;
    return false;
  }
  uint8_t idx = (uint8_t)idxSigned;
  const ModuleDescriptor *m = mods_[idx];

  if (!enabled_[idx]) {
    *errCode = "EDISABLED";
    snprintf(errBuf_, sizeof(errBuf_), "module '%s' is disabled; enable it first", m->id);
    *errMsg = errBuf_;
    return false;
  }

  if (m->dispatch == nullptr) {
    *errCode = "ENOACT";
    snprintf(errBuf_, sizeof(errBuf_), "module '%s' takes no actions", m->id);
    *errMsg = errBuf_;
    return false;
  }

  if (act == nullptr) {
    *errCode = "ENOACT";
    snprintf(errBuf_, sizeof(errBuf_), "missing \"act\" for module '%s'", m->id);
    *errMsg = errBuf_;
    return false;
  }

  return m->dispatch(act, p, d, errCode, errMsg);
}

// ---- persistence ---------------------------------------------------------

void Registry::persist() {
  if (persistSuppressed_) {
    return;
  }

  char out[PERSIST_BUF_SIZE];
  size_t pos = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < count_; i++) {
    if (enabled_[i]) {
      appendf(out, sizeof(out), &pos, "%s%s", pos ? "," : "", mods_[i]->id);
    }
  }

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    report_.nvsWriteOk = false;
    return;
  }
  size_t written = prefs.putString(NVS_KEY, out);
  prefs.end();

  // putString() returns strlen(value), so an empty set legitimately returns 0
  // and cannot be distinguished from a failure that way. Only the non-empty
  // case is checked.
  report_.nvsWriteOk = (pos == 0) || (written == pos);
}

void Registry::restoreFromNvs() {
  report_ = ModuleRestoreReport{};
  report_.nvsWriteOk = true;
  memset(buf_, 0, sizeof(buf_));

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    // The namespace does not exist yet: first boot after a flash or an NVS
    // erase. Not an error, and never a reason to stop booting.
    report_.nvsRead = false;
    return;
  }
  prefs.getString(NVS_KEY, buf_, sizeof(buf_));
  prefs.end();
  report_.nvsRead = true;
  buf_[sizeof(buf_) - 1] = '\0';

  // Split in place. Each token stays a NUL-terminated string inside buf_,
  // which is a member, so report_.unknown[] may safely point into it.
  bool want[MAX_MODULES] = {false};
  char *cur = buf_;
  while (cur != nullptr && *cur != '\0') {
    char *comma = strchr(cur, ',');
    if (comma != nullptr) {
      *comma = '\0';
    }
    if (*cur != '\0') {
      int8_t idx = indexOf(cur);
      if (idx >= 0) {
        want[idx] = true;
      } else if (report_.unknownCount < ModuleRestoreReport::MAX_LIST) {
        // A persisted id we no longer build. Recorded, then ignored.
        report_.unknown[report_.unknownCount++] = cur;
      }
    }
    cur = (comma != nullptr) ? comma + 1 : nullptr;
  }

  // Replay in REGISTRATION order, not NVS order, so the outcome of an
  // unsatisfiable set is deterministic instead of depending on how the string
  // happened to be written.
  //
  // persist() is suppressed for the duration: this is replaying stored intent,
  // not changing it. A module skipped this boot therefore stays in the
  // persisted set and comes back on its own once the conflict is gone. The
  // first explicit enable/disable rewrites the set to what is actually live.
  persistSuppressed_ = true;
  for (uint8_t i = 0; i < count_; i++) {
    if (!want[i]) {
      continue;
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
  persistSuppressed_ = false;
}
