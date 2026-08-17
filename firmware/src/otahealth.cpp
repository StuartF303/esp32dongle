#include "otahealth.h"

#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <stdio.h>
#include <string.h>

#include "activity.h"
#include "bus.h"
#include "console.h"
#include "otadecide.h"
#include "partition_info.h"
#include "registry.h"

namespace {

// ---- threading -----------------------------------------------------------
//
// No lock, and the reasoning rather than the assertion:
//
//   * begin() and tick() run on the loop task only. Every WRITE to the state
//     below happens on one of those two, so the state machine never races
//     itself.
//   * fillStatus() can run on esp_http_server's task (a GET/POST that reaches
//     Console::execute()). It only READS these scalars, all of which are
//     word-sized and naturally atomic on this core, so the worst case is one
//     response reporting the phase from 250 ms ago. It calls
//     registry.isEnabled(), which takes the registry's own lock.
//   * confirmNow() and rollbackNow() WRITE, and could in principle collide with
//     a tick — except both are AUTH_PHYSICAL (cmdauth.h OTA_MUTATE), i.e. CDC
//     only, i.e. the loop task. A network session cannot reach them. If a
//     transport ever dispatches at PHYSICAL off-task, that changes and this
//     note stops being true.
//
// The same trade activity.h makes (backlog C6), for the same reason: a mutex
// here would buy nothing a stale read does not already tolerate.

// ---- state ---------------------------------------------------------------

enum Phase : uint8_t {
  PHASE_IDLE = 0,        // not PENDING_VERIFY at boot. The normal case.
  PHASE_PENDING = 1,     // PENDING_VERIFY, health check running
  PHASE_CONFIRMED = 2,   // mark-valid succeeded this boot
  PHASE_FAILED = 3,      // mark-valid or mark-invalid returned an error; we are stuck
};

Phase phase_ = PHASE_IDLE;
uint32_t ticks_ = 0;
// The state read ONCE, in begin(). Deliberately not re-read every tick: after
// esp_ota_mark_app_valid_cancel_rollback() the partition reports VALID, and a
// tick loop that re-derived "pending" from flash would flip to IDLE and lose
// the fact that this boot confirmed something. fillStatus() re-reads it for
// display, which is a different question.
esp_ota_img_states_t bootState_ = ESP_OTA_IMG_UNDEFINED;
bool bootStateKnown_ = false;
uint8_t lastCriteria_ = 0;
// millis() at the moment the verdict landed, for the event and the `ota`
// command. 0 means "has not happened".
uint32_t decidedAtMs_ = 0;
const char *lastErr_ = nullptr;  // esp_err_to_name() of a failed mark-valid / mark-invalid

// ---- activity.h reporting ------------------------------------------------
//
// The LCD costs NOTHING here. mod_display.cpp's footer already renders
// whatever activity.h is holding, on both screens, with its existing dirty
// tracking and no new region — so "OTA verify in progress" is a producer-side
// call, not a display change. See the report in activity.h for why the
// dependency runs this way.
constexpr char ACT_WHO[] = "ota";
constexpr char ACT_VERB[] = "verify";
bool actOwned_ = false;

// activity.h has ONE producer at a time and no lock (backlog C6). `storage`'s
// verify is the other producer, and it is perfectly legal for someone to start
// one during the 30 s confirmation window. So check we still own the slot
// before touching it, rather than stamping our percentage onto somebody else's
// label.
bool stillOurs() {
  if (!actOwned_) {
    return false;
  }
  Activity::Snapshot a;
  if (!Activity::snapshot(a) || a.state != Activity::RUNNING) {
    return false;
  }
  return strcmp(a.who, ACT_WHO) == 0 && strcmp(a.verb, ACT_VERB) == 0;
}

void actBegin() {
  Activity::begin(ACT_WHO, ACT_VERB);
  actOwned_ = Activity::subscribed();
}

void actEnd(bool ok) {
  if (stillOurs()) {
    Activity::end(ok);
  }
  actOwned_ = false;
}

// ---- partitions ----------------------------------------------------------

// The slot a rollback would land in.
//
// esp_ota_get_next_update_partition(NULL) walks the OTA slots round-robin from
// the running one and is documented never to return the running partition.
// With exactly TWO app slots — see partitions.csv, app0/ota_0 at 0x20000 and
// app1/ota_1 at 0x420000 — "the next slot to write" and "the slot a rollback
// goes to" are the same partition. That equivalence is a property of this
// partition table, not of the API, so it is asserted at runtime rather than
// assumed: with a third slot the two would diverge and this comment would be a
// lie.
const esp_partition_t *rollbackTarget() {
  if (esp_ota_get_app_partition_count() != 2) {
    return nullptr;
  }
  return esp_ota_get_next_update_partition(NULL);
}

// ---- bus events ----------------------------------------------------------

struct EventData {
  const char *reason;     // "health" | "forced" | "window" | "error"
  const char *partition;  // the running partition's label
  const char *target;     // rollback destination label, or nullptr
  const char *err;        // esp_err_to_name(), or nullptr
  uint32_t uptimeMs;
  uint8_t criteria;
};

void fillEvent(JsonObject d, void *ctx) {
  const EventData *e = (const EventData *)ctx;
  if (e == nullptr) {
    return;
  }
  d["reason"] = e->reason;
  d["partition"] = e->partition;
  if (e->target != nullptr) {
    d["target"] = e->target;
  }
  if (e->err != nullptr) {
    d["err"] = e->err;
  }
  d["uptime_ms"] = e->uptimeMs;
  // The mask, expanded. A bare number would need the reader to have this file
  // open; the whole point of putting this on the bus is that the boot log
  // explains itself.
  JsonObject c = d["criteria"].to<JsonObject>();
  for (uint8_t i = 0; i < OtaDecide::CRIT_COUNT; i++) {
    c[OtaDecide::critName(OtaDecide::CRIT_BITS[i])] = (e->criteria & OtaDecide::CRIT_BITS[i]) != 0;
  }
}

const char *runningLabel() {
  const esp_partition_t *p = esp_ota_get_running_partition();
  return (p != nullptr) ? p->label : "?";
}

// ---- the two transitions -------------------------------------------------

// Returns true if the image is now VALID.
bool doConfirm(const char *reason) {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  decidedAtMs_ = millis();
  if (err != ESP_OK) {
    phase_ = PHASE_FAILED;
    lastErr_ = esp_err_to_name(err);
    actEnd(false);
    EventData e = {"error", runningLabel(), nullptr, lastErr_, decidedAtMs_, lastCriteria_};
    Bus::emit("ota.confirm_failed", fillEvent, &e);
    return false;
  }
  phase_ = PHASE_CONFIRMED;
  lastErr_ = nullptr;
  actEnd(true);
  EventData e = {reason, runningLabel(), nullptr, nullptr, decidedAtMs_, lastCriteria_};
  Bus::emit("ota.confirmed", fillEvent, &e);
  return true;
}

// Returns false only on refusal/failure — on success it does not return.
bool doRollback(const char *reason, const char **code, char *msg, size_t cap) {
  const esp_partition_t *target = rollbackTarget();

  // The guard rail. IDF's own call refuses too (ESP_ERR_OTA_ROLLBACK_FAILED),
  // but "rolling back into an erased slot" is precisely how a device with a
  // working image becomes a USB-recovery job, so this says WHICH thing is
  // missing rather than handing back an error number.
  if (!esp_ota_check_rollback_is_possible()) {
    if (code != nullptr) {
      *code = "EROLLBACK";
    }
    if (msg != nullptr && cap > 0) {
      esp_app_desc_t desc;
      bool hasApp = (target != nullptr) && (esp_ota_get_partition_description(target, &desc) == ESP_OK);
      esp_ota_img_states_t tstate;
      const char *tstateName = (target != nullptr && esp_ota_get_state_partition(target, &tstate) == ESP_OK)
                                   ? otaStateName(tstate)
                                   : "no otadata record";
      if (target == nullptr) {
        snprintf(msg, cap, "refused: no second app slot to roll back into");
      } else if (hasApp) {
        // The trap stuart will hit if he flashes app1 with esptool and stops
        // there: the image is present and intact, but otadata has never
        // recorded it as bootable, so neither IDF nor the bootloader will
        // choose it.
        snprintf(msg, cap, "refused: '%s' holds an app but otadata says %s, not bootable", target->label, tstateName);
      } else {
        snprintf(msg, cap, "refused: '%s' contains no valid app image", target->label);
      }
    }
    return false;
  }

  actEnd(false);
  decidedAtMs_ = millis();
  EventData e = {reason, runningLabel(), target != nullptr ? target->label : nullptr, nullptr, decidedAtMs_,
                 lastCriteria_};
  Bus::emit("ota.rollback", fillEvent, &e);
  // Give the CDC/WS sinks a moment to actually push that event out before the
  // chip resets — the same courtesy console.cpp's reboot path extends, and for
  // the same reason: an event nobody received did not happen.
  Serial.flush();
  delay(100);

  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  // Only reached if IDF refused after all.
  phase_ = PHASE_FAILED;
  lastErr_ = esp_err_to_name(err);
  if (code != nullptr) {
    *code = "EROLLBACK";
  }
  if (msg != nullptr && cap > 0) {
    snprintf(msg, cap, "esp_ota_mark_app_invalid_rollback_and_reboot() failed: %s", lastErr_);
  }
  return false;
}

OtaDecide::Inputs gather() {
  OtaDecide::Inputs in;
  in.uptimeMs = millis();
  in.ticks = ticks_;
  // The one restore outcome the registry itself calls fatal: the persisted set
  // was too long to read, so NOTHING was restored and only essential modules
  // are up (see ModuleRestoreReport::nvsTooLong and main.cpp's banner). A
  // skipped or unknown id is not fatal — those are ordinary, and an image that
  // rolled back over them would be rolling back over the owner's own config.
  in.registryFatal = registry.restoreReport().nvsTooLong;
  in.essentialEnabled = registry.isEnabled("cdc");
  in.consoleAnswered = Console::requestsAnswered() > 0;
  return in;
}

}  // namespace

namespace OtaHealth {

void begin() {
  ticks_ = 0;
  phase_ = PHASE_IDLE;
  bootStateKnown_ = false;
  bootState_ = ESP_OTA_IMG_UNDEFINED;
  lastCriteria_ = 0;
  decidedAtMs_ = 0;
  lastErr_ = nullptr;
  actOwned_ = false;

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running == nullptr) {
    return;
  }
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
    // No otadata record for this partition at all — both sectors blank or
    // CRC-invalid. NOT what a USB flash looks like: PlatformIO writes
    // boot_app0.bin at 0x12000, which carries a valid ota_seq=1 / UNDEFINED
    // entry, so the normal path is ESP_OK with UNDEFINED. This branch is an
    // erased or corrupt otadata, and is treated exactly like UNDEFINED —
    // nothing is pending, so there is nothing to confirm.
    return;
  }
  bootState_ = state;
  bootStateKnown_ = true;

  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }

  phase_ = PHASE_PENDING;
  actBegin();

  const esp_partition_t *target = rollbackTarget();
  EventData e = {"boot", running->label, target != nullptr ? target->label : nullptr, nullptr, millis(), 0};
  Bus::emit("ota.pending", fillEvent, &e);
}

void tick() {
  ticks_++;
  if (phase_ != PHASE_PENDING) {
    return;  // every USB flash takes this branch, forever
  }

  OtaDecide::Inputs in = gather();
  lastCriteria_ = OtaDecide::criteriaMet(in, OtaDecide::DEFAULTS);

  switch (OtaDecide::decide(true, in, OtaDecide::DEFAULTS)) {
    case OtaDecide::CONFIRM:
      doConfirm("health");
      break;

    case OtaDecide::ROLLBACK: {
      // The window closed with criteria outstanding. Sitting in PENDING_VERIFY
      // instead is the one option that is not acceptable: the bootloader would
      // roll back anyway at the next restart, whenever that happened to be, and
      // the owner would experience it as "the update vanished" days later with
      // nothing in any log tying it to this boot. Deciding here at least makes
      // it an event.
      char why[96];
      why[0] = '\0';
      if (!doRollback("window", nullptr, why, sizeof(why))) {
        // Refused — there is nothing bootable to land in. PHASE_FAILED, not a
        // retry: re-asking every 250 ms would re-read otadata from flash
        // forever and would never get a different answer, while burying the one
        // event that matters. The image stays PENDING_VERIFY and the bootloader
        // will abort it at the next restart; saying so once, loudly, is all
        // this code can honestly do.
        phase_ = PHASE_FAILED;
        lastErr_ = "rollback refused: no bootable slot";
        decidedAtMs_ = millis();
        actEnd(false);
        EventData e = {"window", runningLabel(), nullptr, why, decidedAtMs_, lastCriteria_};
        Bus::emit("ota.rollback_failed", fillEvent, &e);
      }
      break;
    }

    case OtaDecide::WAIT:
      if (stillOurs()) {
        Activity::progress(OtaDecide::progressPct(in, OtaDecide::DEFAULTS));
      }
      break;

    case OtaDecide::IDLE:
    default:
      break;
  }
}

void fillStatus(JsonObject d) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  d["running"] = (running != nullptr) ? running->label : "?";
  if (running != nullptr) {
    d["running_offset"] = running->address;
  }

  // Re-read rather than reporting bootState_: after a confirm this must say
  // VALID, which is the whole point of asking.
  esp_ota_img_states_t state;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    d["ota_state"] = otaStateName(state);
  } else {
    d["ota_state"] = "?";  // no otadata record at all — erased or corrupt, NOT the post-flash state
  }
  d["boot_ota_state"] = bootStateKnown_ ? otaStateName(bootState_) : "?";

  const char *phase = "idle";
  switch (phase_) {
    case PHASE_PENDING:
      phase = "pending";
      break;
    case PHASE_CONFIRMED:
      phase = "confirmed";
      break;
    case PHASE_FAILED:
      phase = "failed";
      break;
    case PHASE_IDLE:
    default:
      break;
  }
  d["phase"] = phase;
  d["pending"] = phase_ == PHASE_PENDING;
  d["confirmed_this_boot"] = phase_ == PHASE_CONFIRMED;
  if (lastErr_ != nullptr) {
    d["err"] = lastErr_;
  }
  if (decidedAtMs_ != 0) {
    d["decided_at_ms"] = decidedAtMs_;
  }

  OtaDecide::Inputs in = gather();
  uint8_t met = OtaDecide::criteriaMet(in, OtaDecide::DEFAULTS);
  JsonObject crit = d["criteria"].to<JsonObject>();
  for (uint8_t i = 0; i < OtaDecide::CRIT_COUNT; i++) {
    crit[OtaDecide::critName(OtaDecide::CRIT_BITS[i])] = (met & OtaDecide::CRIT_BITS[i]) != 0;
  }
  d["criteria_all_met"] = met == OtaDecide::CRIT_ALL;
  d["uptime_ms"] = in.uptimeMs;
  d["ticks"] = in.ticks;
  d["window_ms"] = OtaDecide::DEFAULTS.windowMs;
  // Only meaningful while pending — a window that keeps counting down on an
  // idle device invites the reading "something is about to happen".
  d["window_remaining_s"] =
      (phase_ == PHASE_PENDING) ? (OtaDecide::remainingMs(in, OtaDecide::DEFAULTS) / 1000u) : 0u;
  d["verdict"] = OtaDecide::verdictName(OtaDecide::decide(phase_ == PHASE_PENDING, in, OtaDecide::DEFAULTS));

  // ---- the other slot ---------------------------------------------------
  JsonObject t = d["rollback_target"].to<JsonObject>();
  const esp_partition_t *target = rollbackTarget();
  if (target == nullptr) {
    // No `label` key at all rather than a JSON null: "there is no second slot"
    // and "the second slot is called null" read very differently to a UI.
    t["has_app"] = false;
  } else {
    t["label"] = target->label;
    t["offset"] = target->address;
    esp_ota_img_states_t tstate;
    t["ota_state"] = (esp_ota_get_state_partition(target, &tstate) == ESP_OK) ? otaStateName(tstate) : "?";
    // Two DIFFERENT questions, both worth answering:
    //   has_app  — is there an image with a valid magic word sitting there?
    //              esptool write-flash of a firmware.bin makes this true.
    //   possible — will IDF and the bootloader actually choose it? That also
    //              needs an otadata entry marking it bootable, which only an
    //              esp_ota_set_boot_partition()/confirm cycle creates.
    // Reporting only the second would leave "I definitely flashed it" looking
    // like a lie; reporting only the first would be dangerously reassuring.
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(target, &desc) == ESP_OK) {
      t["has_app"] = true;
      char built[48];
      snprintf(built, sizeof(built), "%s %s", desc.date, desc.time);
      t["build"] = (const char *)built;  // cast: char[] would be stored by pointer, see console.cpp
      t["idf_version"] = (const char *)desc.idf_ver;
    } else {
      t["has_app"] = false;
    }
  }
  d["rollback_possible"] = esp_ota_check_rollback_is_possible();
}

bool confirmNow(const char **code, char *msg, size_t cap) {
  if (phase_ != PHASE_PENDING) {
    if (code != nullptr) {
      *code = "EOTASTATE";
    }
    if (msg != nullptr && cap > 0) {
      snprintf(msg, cap, "nothing to confirm: this image booted %s, not PENDING_VERIFY",
               bootStateKnown_ ? otaStateName(bootState_) : "with no otadata record");
    }
    return false;
  }
  if (!doConfirm("forced")) {
    if (code != nullptr) {
      *code = "EOTA";
    }
    if (msg != nullptr && cap > 0) {
      snprintf(msg, cap, "esp_ota_mark_app_valid_cancel_rollback() failed: %s",
               lastErr_ != nullptr ? lastErr_ : "?");
    }
    return false;
  }
  if (msg != nullptr && cap > 0) {
    snprintf(msg, cap, "marked valid; rollback cancelled");
  }
  return true;
}

bool rollbackNow(const char **code, char *msg, size_t cap) {
  // Deliberately NOT gated on phase_ == PHASE_PENDING. "This build is bad, go
  // back to the last one" is a legitimate operator action long after the
  // confirmation window has closed, and it is the only manual escape from a
  // build that boots but misbehaves. The guard that matters is whether there is
  // a bootable slot to land in, which doRollback() checks.
  return doRollback("forced", code, msg, cap);
}

}  // namespace OtaHealth
