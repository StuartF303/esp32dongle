#include "console.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "claims_selftest.h"
#include "cmdauth.h"
#include "partition_info.h"
#include "protocol.h"
#include "bootprobe.h"
#include "otahealth.h"
#include "registry.h"
#include "scheduler.h"

namespace {

// ---- line reader: non-blocking, bounded --------------------------------
//
// Protocol::MAX_LINE, not a local number: the WS and BLE adapters in W2 must
// accept exactly the same maximum or a request that works on one transport
// truncates on another. See protocol.h.
char lineBuf[Protocol::MAX_LINE + 1];
size_t lineLen = 0;
bool lineOverflowed = false;

// ---- command dispatch ----------------------------------------------------

// Handler contract: read `p` (may be a null/empty object if the request had
// no "p"), fill `d` and return DISPATCH_OK, or fill `err` and return
// DISPATCH_FAIL. Never blocks except `reboot`, which is a deliberate,
// documented one-off (see dispatch()).
//
// `e` is the error object. It is pre-created and thrown away on success, so a
// handler that returns DISPATCH_FAIL can attach machine-readable detail
// alongside the {code,msg} pair from ARCHITECTURE.md section 2 — `enable` uses
// it for "blocked_by", because a UI cannot parse that out of prose.
//
// Same signature shape as ModuleDispatchFn (registry.h) on purpose: a built-in
// and a module action differ in where they are found, not in how they are
// called.
typedef DispatchResult (*CommandHandler)(const CmdContext &ctx, JsonObjectConst p, JsonObject d, JsonObject e,
                                         CmdError *err);

struct Command {
  const char *name;
  const char *help;
  CommandHandler handler;
  // The AuthLevel a caller must hold for `handler` to run at all. Enforced in
  // execute() BEFORE the handler is called — see the gate there — so no
  // built-in checks auth for itself and none of them can forget to. The value
  // comes from CmdAuth::requiredFor(), which is the single source of truth for
  // the policy (cmdauth.h).
  uint8_t minAuth;
};

// CmdAuth cannot include registry.h (it would stop being host-testable), so it
// carries its own copies of the three levels. These are the only thing keeping
// the two definitions the same value.
static_assert(CmdAuth::NONE == AUTH_NONE, "CmdAuth::NONE has drifted from AuthLevel");
static_assert(CmdAuth::TOKEN == AUTH_TOKEN, "CmdAuth::TOKEN has drifted from AuthLevel");
static_assert(CmdAuth::PHYSICAL == AUTH_PHYSICAL, "CmdAuth::PHYSICAL has drifted from AuthLevel");

// Serialises access to Serial for WHOLE lines.
//
// W2 made this necessary: Bus::emit() can now run on the HTTP server task (a
// module dispatched from POST /api/cmd emits an event), while the loop task is
// half-way through writing a heartbeat event or a command response. Serial is
// a byte stream with no framing of its own, so two interleaved writes produce
// one corrupt line and every host-side line parser — tools/console.py
// included — chokes on it. serializeJson() makes many small write() calls, so
// the window is wide, not theoretical.
//
// Non-recursive: nothing here writes a line from inside a line. Created in
// begin(); a null handle degrades to no locking rather than crashing, matching
// the registry's stance.
SemaphoreHandle_t txLock = nullptr;

// Requests answered by execute(), any transport. See Console::requestsAnswered()
// in console.h.
//
// std::atomic rather than `volatile`: execute() runs on the loop task AND on
// esp_http_server's task, and `volatile` gives ordering without atomicity — the
// read-modify-write of a plain counter can still lose an increment across two
// cores, and C++20 deprecates `++` on a volatile for exactly that reason
// (-Wvolatile). relaxed ordering is enough: nothing else is published through
// this counter, and every consumer only asks whether it is non-zero.
std::atomic<uint32_t> requestsAnswered_{0};

void sendLine(JsonDocument &doc) {
  if (txLock != nullptr) {
    xSemaphoreTake(txLock, portMAX_DELAY);
  }
  serializeJson(doc, Serial);
  Serial.print('\n');
  if (txLock != nullptr) {
    xSemaphoreGive(txLock);
  }
}

void sendErrorNoId(const char *code, const char *msg) {
  JsonDocument resp;
  resp["ok"] = false;
  JsonObject e = resp["e"].to<JsonObject>();
  e["code"] = code;
  e["msg"] = msg;
  sendLine(resp);
}

// ---- event sink ----------------------------------------------------------

// This transport's Bus subscription. Registered in begin(); modules emit via
// Bus::emit() and never mention the console.
void eventSink(const char *name, Bus::FillFn fill, void *ctx) {
  JsonDocument doc;
  doc["ev"] = name;
  JsonObject d = doc["d"].to<JsonObject>();
  if (fill != nullptr) {
    fill(d, ctx);
  }
  sendLine(doc);
}

// ---- handlers --------------------------------------------------------

// Defined after the COMMANDS table, which it walks — the table cannot be
// forward-declared any more (it is constexpr, so the static_asserts below it
// can read it), so the FUNCTION is forward-declared instead.
DispatchResult cmdHelp(const CmdContext &ctx, JsonObjectConst p, JsonObject d, JsonObject e, CmdError *err);

// Reports what the static-constructor probe observed. See bootprobe.h: this is
// the only evidence that hid's arming mechanism works on real silicon, because
// its failure mode (no interface bound) is indistinguishable from "not armed".
DispatchResult cmdBootProbe(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  d["ctor_ran"] = BootProbe::ctorRan;
  d["nvs_init_err"] = (int32_t)BootProbe::nvsInitErr;
  d["nvs_init_ok"] = (BootProbe::nvsInitErr == 0);
  d["led_armed_at_boot"] = BootProbe::ledArmed;
  d["led_enabled_now"] = registry.isEnabled("led");
  return DISPATCH_OK;
}

DispatchResult cmdInfo(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  esp_chip_info_t info;
  esp_chip_info(&info);
  d["chip"] = ESP.getChipModel();

  char rev[16];
  snprintf(rev, sizeof(rev), "v%d.%d", info.revision / 100, info.revision % 100);
  d["rev"] = rev;

  d["cores"] = info.cores;
  d["cpu_mhz"] = ESP.getCpuFreqMHz();
  d["flash_bytes"] = ESP.getFlashChipSize();

  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  d["mac"] = macStr;

  const esp_app_desc_t *app = esp_app_get_description();
  d["idf_version"] = app->idf_ver;
  char built[48];
  snprintf(built, sizeof(built), "%s %s", app->date, app->time);
  d["build"] = built;

  const esp_partition_t *running = esp_ota_get_running_partition();
  d["running_partition"] = running ? running->label : "?";

  esp_ota_img_states_t otaState;
  if (running && esp_ota_get_state_partition(running, &otaState) == ESP_OK) {
    d["ota_state"] = otaStateName(otaState);
  } else {
    d["ota_state"] = "?";
  }

  return DISPATCH_OK;
}

DispatchResult cmdParts(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  JsonArray parts = d["partitions"].to<JsonArray>();

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL) {
    const esp_partition_t *part = esp_partition_get(it);
    JsonObject o = parts.add<JsonObject>();
    o["label"] = part->label;
    o["type"] = part->type == ESP_PARTITION_TYPE_APP ? "app" : "data";
    o["subtype"] = partitionSubtypeName(part);
    o["offset"] = part->address;
    o["size"] = part->size;
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);

  return DISPATCH_OK;
}

DispatchResult cmdMem(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  d["free_heap"] = ESP.getFreeHeap();
  d["min_free_heap"] = ESP.getMinFreeHeap();
  d["largest_free_block"] = ESP.getMaxAllocHeap();
  return DISPATCH_OK;
}

DispatchResult cmdUptime(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  d["uptime_ms"] = millis();
  return DISPATCH_OK;
}

DispatchResult cmdTasks(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  JsonArray arr = d["tasks"].to<JsonArray>();
  for (uint8_t i = 0; i < scheduler.taskCount(); i++) {
    const SchedulerTaskStats &s = scheduler.statsAt(i);
    JsonObject o = arr.add<JsonObject>();
    o["name"] = s.name;
    o["interval_ms"] = s.intervalMs;
    o["last_run_ms"] = s.lastRunMs;
    o["run_count"] = s.runCount;
    o["last_us"] = s.lastDurationUs;
    o["worst_us"] = s.worstDurationUs;
  }
  return DISPATCH_OK;
}

// `led` is a convenience alias for {"mod":"led","act":"set"} and owns no state
// of its own — the LED belongs to the `led` module, which claims
// Claims::RES_LED exclusively. Disabling that module makes this command fail
// with EDISABLED, which is the point of the exercise.
DispatchResult cmdLed(const CmdContext &ctx, JsonObjectConst p, JsonObject d, JsonObject, CmdError *err) {
  return registry.dispatch("led", "set", ctx, p, d, err);
}

// ---- module registry commands -------------------------------------------

// NOTE: no static scratch buffer here any more. The old one existed because of
// a comment claiming ArduinoJson stores a `const char *` by reference; that is
// wrong for ArduinoJson 7.4.3. Only string LITERALS are linked
// (StringAdapter<const char(&)[N]> passes isStatic=true); a runtime
// `const char *` or a `char[]` goes through saveString() and is COPIED into
// the document. r.msg is a char[192] on the caller's stack and copying it into
// `d` is safe. W2's adapters must not reproduce the old buffer.
DispatchResult renderActionResult(const char *id, const ModuleActionResult &r, JsonObject d, JsonObject e,
                                  CmdError *err) {
  // `d` is filled on BOTH paths. What was stopped is a fact about the device,
  // not a detail of an error, and a caller that has to look in two different
  // places for it will eventually look in only one.
  d["id"] = id;
  d["enabled"] = r.enabledAfter;
  d["changed"] = r.changed;  // false == it was already in that state
  // The (const char *) cast is LOAD-BEARING, not style. ArduinoJson 7.4.3 has a
  // separate adapter for char arrays: StringAdapter<const char (&)[N]> returns
  // RamString(p, N-1, /*isStatic=*/true), which stores a POINTER and assumes the
  // storage is a string literal that outlives the document. r.msg is char[192] in
  // a caller stack frame, so `d["msg"] = r.msg` left the document referencing dead
  // stack and serialised garbage ("Ц ...") once this function returned.
  // Casting to const char * selects the RamString(str, strlen(str)) overload with
  // isStatic=false, which copies. Observed and fixed 2026-08-16.
  // Any transport rendering a fixed char[] buffer into JSON needs the same cast.
  d["msg"] = (const char *)r.msg;  // full text; e.msg below is capped at CmdError::msg
  if (r.pendingRestart) {
    d["pending_restart"] = true;
    if (r.code != nullptr) {
      d["code"] = r.code;  // "EREBOOT" — the one code that rides along with ok:true
    }
  }
  JsonArray stopped = d["stopped"].to<JsonArray>();
  for (uint8_t i = 0; i < r.stoppedCount; i++) {
    stopped.add(r.stopped[i]);
  }
  JsonArray failed = d["failed_to_stop"].to<JsonArray>();
  for (uint8_t i = 0; i < r.failedToStopCount; i++) {
    failed.add(r.failedToStop[i]);
  }

  if (r.ok) {
    return DISPATCH_OK;
  }

  cmdErrorf(err, r.code != nullptr ? r.code : "EFAIL", "%s", r.msg);
  e["id"] = id;
  e["enabled"] = r.enabledAfter;
  if (r.blockedByCount > 0) {
    JsonArray blocked = e["blocked_by"].to<JsonArray>();
    for (uint8_t i = 0; i < r.blockedByCount; i++) {
      blocked.add(r.blockedBy[i]);
    }
  }
  return DISPATCH_FAIL;
}

// The exact shape GET /api/modules serves — literally, since W2: mod_http.cpp
// calls Console::fillModules() rather than re-rendering the registry itself.
DispatchResult cmdModules(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  Console::fillModules(d);
  return DISPATCH_OK;
}

DispatchResult cmdEnable(const CmdContext &, JsonObjectConst p, JsonObject d, JsonObject e, CmdError *err) {
  const char *id = p["id"] | (const char *)nullptr;
  if (id == nullptr) {
    cmdErrorf(err, "EARGS", "missing p.id (module id, see the `modules` command)");
    return DISPATCH_FAIL;
  }
  bool force = p["force"] | false;

  ModuleActionResult r;
  registry.enable(id, force, r);
  return renderActionResult(id, r, d, e, err);
}

DispatchResult cmdDisable(const CmdContext &, JsonObjectConst p, JsonObject d, JsonObject e, CmdError *err) {
  const char *id = p["id"] | (const char *)nullptr;
  if (id == nullptr) {
    cmdErrorf(err, "EARGS", "missing p.id (module id, see the `modules` command)");
    return DISPATCH_FAIL;
  }

  ModuleActionResult r;
  registry.disable(id, r);
  return renderActionResult(id, r, d, e, err);
}

// Runs the assertion table in claims_selftest.h against the pure arbitration
// function. Synthetic ClaimSets only — no fake modules are ever registered in
// the live registry, so this cannot perturb real hardware state.
//
// A failing assertion is now a FAILED command (ESELFTEST), not an ok:true with
// a non-zero counter buried in `d` that a script would have to know to look
// at. The counts survive the failure because `d` is no longer discarded on an
// error response.
DispatchResult cmdSelftest(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *err) {
  Claims::SelfTestResult res = Claims::runSelfTest();
  d["cases"] = res.total;
  d["passed"] = res.passed;
  d["failed"] = res.failed;
  JsonArray failures = d["failures"].to<JsonArray>();
  for (uint8_t i = 0; i < res.namedFailures; i++) {
    failures.add(res.failures[i]);
  }
  d["failures_truncated"] = res.failed > res.namedFailures;

  if (res.failed > 0) {
    cmdErrorf(err, "ESELFTEST", "%u of %u claim arbitration cases FAILED — see d.failures", (unsigned)res.failed,
              (unsigned)res.total);
    return DISPATCH_FAIL;
  }
  return DISPATCH_OK;
}

const char *const LOG_LEVELS[] = {"error", "warn", "info", "debug", "trace"};
const size_t LOG_LEVEL_COUNT = sizeof(LOG_LEVELS) / sizeof(LOG_LEVELS[0]);
uint8_t currentLogLevel = 2;  // "info"

int8_t logLevelIndex(const char *name) {
  for (uint8_t i = 0; i < LOG_LEVEL_COUNT; i++) {
    if (strcasecmp(name, LOG_LEVELS[i]) == 0) {
      return (int8_t)i;
    }
  }
  return -1;
}

// Get/set only, in this slice — nothing yet reads this to filter its own
// output (there's no logging subsystem beyond direct Serial writes yet).
// This is plumbing for later modules; flagged in the implementation report.
DispatchResult cmdLog(const CmdContext &, JsonObjectConst p, JsonObject d, JsonObject, CmdError *err) {
  const char *level = p["level"] | (const char *)nullptr;
  if (level) {
    int8_t idx = logLevelIndex(level);
    if (idx < 0) {
      cmdErrorf(err, "EARGS", "level must be one of error/warn/info/debug/trace");
      return DISPATCH_FAIL;
    }
    currentLogLevel = (uint8_t)idx;
  }
  d["level"] = LOG_LEVELS[currentLogLevel];
  return DISPATCH_OK;
}

DispatchResult cmdReboot(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  d["restarting"] = true;
  return DISPATCH_OK;  // dispatch() actually restarts, after this response is on the wire
}

// ---- ota (backlog S4) ----------------------------------------------------
//
// Read-only with no params. The three mutating params — confirm, rollback and
// boot — are gated at CmdAuth::OTA_MUTATE (PHYSICAL) here in the handler rather
// than by the row in CmdAuth::BUILTINS — the ONLY built-in that does this, and
// the reasoning is written down beside that constant in cmdauth.h.
//
// The refusal goes through the same CmdAuth::denyMessage() the central gate
// uses, so `ota p:{rollback:true}` from a phone is byte-for-byte
// indistinguishable from the refusal `reboot` gives it.
DispatchResult cmdOta(const CmdContext &ctx, JsonObjectConst p, JsonObject d, JsonObject, CmdError *err) {
  bool confirm = p["confirm"] | false;
  bool rollback = p["rollback"] | false;

  // p:{boot:"app1"} — which slot the bootloader starts next. Presence, not
  // truthiness: p:{boot:1} is a mistake worth reporting, so it arrives here as
  // "asked, with no usable label" rather than being silently ignored.
  bool bootAsked = !p["boot"].isNull();
  const char *bootLabel = bootAsked ? (p["boot"] | (const char *)nullptr) : nullptr;

  // Bare-word shorthand, so `ota confirm` / `ota rollback` / `ota boot app1`
  // are typeable on the CDC console like every other built-in
  // (handleBareWord() puts the rest of the line in p.arg). Only these exact
  // words; anything else is IGNORED rather than guessed at, exactly as
  // `enable`'s optional "force" is — a destructive command must not be
  // reachable by a near miss.
  const char *arg = p["arg"] | (const char *)nullptr;
  if (arg != nullptr) {
    confirm = confirm || strcmp(arg, "confirm") == 0;
    rollback = rollback || strcmp(arg, "rollback") == 0;
    // "boot app1" arrives as one string because the shim is deliberately thin
    // (one key, the whole remainder). Splitting it HERE rather than teaching
    // handleBareWord() a third special case keeps that shim thin. A bare
    // `ota boot` yields the empty label, which setBootNow() refuses with the
    // usage line — better than printing status and looking like it worked.
    if (strncmp(arg, "boot", 4) == 0 && (arg[4] == '\0' || arg[4] == ' ')) {
      const char *rest = arg + 4;
      while (*rest == ' ') {
        rest++;
      }
      bootAsked = true;
      bootLabel = rest;
    }
  }

  // Status FIRST, on every path including the refusals below: a caller told
  // "no" still needs to see the state it was reasoning about, and `d` survives
  // an error response (ARCHITECTURE.md section 2).
  OtaHealth::fillStatus(d);

  const uint8_t asked = (confirm ? 1 : 0) + (rollback ? 1 : 0) + (bootAsked ? 1 : 0);
  if (asked == 0) {
    return DISPATCH_OK;
  }
  if (asked > 1) {
    cmdErrorf(err, "EARGS", "confirm, rollback and boot are three different decisions; ask for one");
    return DISPATCH_FAIL;
  }
  if (!CmdAuth::permits(ctx.authLevel, CmdAuth::OTA_MUTATE)) {
    char what[48];
    snprintf(what, sizeof(what), "'ota %s'", bootAsked ? "boot" : (confirm ? "confirm" : "rollback"));
    char msg[sizeof(CmdError::msg)];
    CmdAuth::denyMessage(msg, sizeof(msg), what, CmdAuth::OTA_MUTATE, ctx.transport, ctx.authLevel);
    cmdErrorf(err, "EAUTH", "%s", msg);
    return DISPATCH_FAIL;
  }

  const char *code = "EOTA";
  char msg[sizeof(CmdError::msg)];
  msg[0] = '\0';
  OtaHealth::BootSetReport rep;
  bool ok;
  if (bootAsked) {
    // Unlike the other two this one RETURNS. Selecting the next image and
    // restarting into it are separate decisions on purpose — `reboot` is a
    // second, equally PHYSICAL command.
    ok = OtaHealth::setBootNow(bootLabel, rep, &code, msg, sizeof(msg));
  } else if (confirm) {
    ok = OtaHealth::confirmNow(&code, msg, sizeof(msg));
  } else {
    // rollbackNow() does not return on success — the chip restarts into the
    // other slot. That is why there is no "restarting" response to render:
    // unlike `reboot`, this one cannot be deferred until after the reply,
    // because the decision and the restart are a single IDF call.
    ok = OtaHealth::rollbackNow(&code, msg, sizeof(msg));
  }
  if (!ok) {
    cmdErrorf(err, code, "%s", msg);
    return DISPATCH_FAIL;
  }
  // Re-read: `d` was filled before the transition, so it still says "pending"
  // (and, for boot, still names the OLD boot partition).
  d.clear();
  OtaHealth::fillStatus(d);
  if (bootAsked) {
    OtaHealth::fillBootSet(d["boot_set"].to<JsonObject>(), rep);
  }
  d["msg"] = (const char *)msg;  // cast: char[] is stored by pointer, see renderActionResult()
  return DISPATCH_OK;
}

// CONSTEXPR so the policy static_asserts below can read it. Every row's
// minAuth comes from CmdAuth::requiredFor(name) rather than a literal, so the
// level for a command is written down exactly once, in cmdauth.h.
constexpr Command COMMANDS[] = {
    {"help", "list commands, each with the auth level it needs", cmdHelp, CmdAuth::requiredFor("help")},
    {"info", "chip/build/partition identity", cmdInfo, CmdAuth::requiredFor("info")},
    {"parts", "live partition table", cmdParts, CmdAuth::requiredFor("parts")},
    {"mem", "heap free / min-free / largest block", cmdMem, CmdAuth::requiredFor("mem")},
    {"uptime", "ms since boot", cmdUptime, CmdAuth::requiredFor("uptime")},
    {"tasks", "scheduler task table (intervals, worst-case runtimes)", cmdTasks, CmdAuth::requiredFor("tasks")},
    {"led", "alias for mod:\"led\" act:\"set\"; p:{rgb:\"rrggbb\"} or p:{rgb:\"off\"}", cmdLed,
     CmdAuth::requiredFor("led")},
    {"modules", "registered modules: id, name, category, enabled, claims, actions, blocked_by, status", cmdModules,
     CmdAuth::requiredFor("modules")},
    {"enable", "enable a module; p:{id:\"led\"[,force:true]}", cmdEnable, CmdAuth::requiredFor("enable")},
    {"disable", "disable a module; p:{id:\"led\"}", cmdDisable, CmdAuth::requiredFor("disable")},
    {"selftest", "assert the claim arbitration rule against a synthetic case table", cmdSelftest,
     CmdAuth::requiredFor("selftest")},
    {"log", "get/set runtime log level; p:{level:\"error|warn|info|debug|trace\"}", cmdLog,
     CmdAuth::requiredFor("log")},
    {"bootprobe", "what the static-ctor NVS probe saw (hid arming depends on it)", cmdBootProbe,
     CmdAuth::requiredFor("bootprobe")},
    {"ota",
     "rollback state + health criteria; p:{confirm:true} | p:{rollback:true} | p:{boot:\"app0|app1\"} — select "
     "without rebooting (auth >= physical)",
     cmdOta, CmdAuth::requiredFor("ota")},
    {"reboot", "esp_restart() — responds first (auth >= physical: the cable, not the network)", cmdReboot,
     CmdAuth::requiredFor("reboot")},
};
constexpr size_t COMMAND_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

// The two ways the table and the policy could drift, both made into build
// failures rather than a silent AUTH_PHYSICAL (or, worse, a level nobody
// chose).
constexpr bool everyCommandHasAPolicy() {
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    if (!CmdAuth::isListed(COMMANDS[i].name)) {
      return false;
    }
  }
  return true;
}
static_assert(everyCommandHasAPolicy(),
              "a built-in command has no entry in CmdAuth::BUILTINS — add one (cmdauth.h) rather than letting it "
              "default to AUTH_PHYSICAL");
static_assert(COMMAND_COUNT == CmdAuth::BUILTIN_COUNT,
              "CmdAuth::BUILTINS names a command that no longer exists in COMMANDS, or vice versa");

// Every command's level, and whether THIS caller has it — so a UI can grey out
// what the session cannot use instead of discovering it by failure. `auth` is
// the caller's own level, which is otherwise invisible to it.
DispatchResult cmdHelp(const CmdContext &ctx, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  d["auth"] = CmdAuth::levelName(ctx.authLevel);
  d["auth_level"] = ctx.authLevel;
  JsonArray cmds = d["commands"].to<JsonArray>();
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    JsonObject o = cmds.add<JsonObject>();
    o["act"] = COMMANDS[i].name;
    o["help"] = COMMANDS[i].help;
    o["min_auth"] = CmdAuth::levelName(COMMANDS[i].minAuth);
    o["allowed"] = CmdAuth::permits(ctx.authLevel, COMMANDS[i].minAuth);
  }
  return DISPATCH_OK;
}

const Command *findCommand(const char *name) {
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    if (strcmp(COMMANDS[i].name, name) == 0) {
      return &COMMANDS[i];
    }
  }
  return nullptr;
}

// Fills `resp` with the refusal for a built-in the caller may not run, and
// returns false (the "do not restart" value execute() hands back).
//
// It goes through cmdErrorf() and CmdError::msg — the SAME path a module's
// EAUTH takes (mod_hid.cpp requireInjectAuth, mod_storage.cpp requireAuth) —
// so the code, the response shape, the wording and even the 96-byte truncation
// point are identical. A caller must not be able to tell a built-in's refusal
// from a module's; anything extra in `e` here would give it away.
bool authDenied(JsonDocument &resp, const char *what, uint8_t need, const CmdContext &ctx) {
  // Formatted into a SEPARATE buffer first: cmdErrorf() vsnprintf()s into
  // cerr.msg, and passing cerr.msg as its own argument would be an overlapping
  // copy. Same size, so the truncation point is unchanged.
  char msg[sizeof(CmdError::msg)];
  CmdAuth::denyMessage(msg, sizeof(msg), what, need, ctx.transport, ctx.authLevel);
  CmdError cerr;
  cmdErrorf(&cerr, "EAUTH", "%s", msg);
  resp["ok"] = false;
  JsonObject e = resp["e"].to<JsonObject>();
  e["code"] = cerr.code;
  // Cast is load-bearing: cerr.msg is a char[96] on this stack frame, and
  // ArduinoJson 7.4.3 stores a char-array by POINTER without copying. See the
  // note above renderActionResult().
  e["msg"] = (const char *)cerr.msg;
  return false;
}

// ---- request handling --------------------------------------------------

// The CDC transport's use of the shared core. Everything CDC-specific lives
// here: the AUTH_PHYSICAL context (someone with the cable in their hand can
// already reflash the device, so gating them lower would be theatre), writing
// the line, and the reboot flush.
void dispatch(JsonObjectConst req) {
  JsonDocument resp;
  bool restartNow = Console::execute(req, AUTH_PHYSICAL, "cdc", resp);
  sendLine(resp);

  if (restartNow) {
    // Terminal action, not a scheduler task: the loop is ending for good, so
    // the "never delay() in the main path" rule doesn't apply here. This gives
    // the USB CDC TX queue a moment to actually get the response out before
    // the chip resets.
    Serial.flush();
    delay(100);
    esp_restart();
  }
}

void handleJsonLine(const char *json) {
  JsonDocument reqDoc;
  DeserializationError err = deserializeJson(reqDoc, json);
  if (err) {
    sendErrorNoId("EPARSE", err.c_str());
    return;
  }
  dispatch(reqDoc.as<JsonObjectConst>());
}

// Bare-word shim: `act [rest]` -> {"act":act,"p":{key:rest}}. Deliberately
// thin — no validation here, handlers reject bad params themselves.
void handleBareWord(char *word) {
  char *sp = strchr(word, ' ');
  const char *act = word;
  char *rest = nullptr;

  if (sp) {
    *sp = '\0';
    rest = sp + 1;
    while (*rest == ' ' || *rest == '\t') {
      rest++;
    }
  }

  JsonDocument reqDoc;
  reqDoc["act"] = act;
  if (rest && *rest) {
    bool isToggle = strcmp(act, "enable") == 0 || strcmp(act, "disable") == 0;
    if (isToggle) {
      // `enable led force` -> {"act":"enable","p":{"id":"led","force":true}}.
      // Anything other than the literal "force" after the id is ignored
      // rather than guessed at — force must be asked for explicitly.
      char *sp2 = strchr(rest, ' ');
      if (sp2) {
        *sp2 = '\0';
        if (strcmp(sp2 + 1, "force") == 0) {
          reqDoc["p"]["force"] = true;
        }
      }
      reqDoc["p"]["id"] = rest;
    } else {
      const char *key = strcmp(act, "led") == 0 ? "rgb" : (strcmp(act, "log") == 0 ? "level" : "arg");
      reqDoc["p"][key] = rest;
    }
  }

  dispatch(reqDoc.as<JsonObjectConst>());
}

void processLine(char *raw) {
  char *start = raw;
  while (*start == ' ' || *start == '\t') {
    start++;
  }
  if (*start == '\0') {
    return;  // blank line, ignore silently
  }

  if (*start == '{') {
    handleJsonLine(start);
  } else {
    handleBareWord(start);
  }
}

}  // namespace

namespace Console {

void begin() {
  lineLen = 0;
  lineOverflowed = false;
  if (txLock == nullptr) {
    txLock = xSemaphoreCreateMutex();
  }
  Bus::addSink(eventSink);
}

// ---- the shared request -> response core --------------------------------
//
// See console.h for the contract. This is the ONLY dispatcher in the image;
// CDC, HTTP and (later) BLE all funnel through it, which is what makes "the
// REST mirror cannot drift from the console" a structural property rather than
// a promise.
bool execute(JsonObjectConst req, uint8_t authLevel, const char *transport, JsonDocument &resp) {
  resp.clear();

  // Counted HERE, at the top, because every path out of this function fills
  // `resp` — including ENOACT, EAUTH and EUNKNOWN. The counter's consumer
  // (otahealth.cpp) is asking whether the command core ran end to end, and a
  // refusal proves that just as well as a success does.
  requestsAnswered_.fetch_add(1, std::memory_order_relaxed);

  JsonVariantConst idVar = req["id"];
  const char *act = req["act"] | (const char *)nullptr;
  JsonObjectConst params = req["p"];

  if (!idVar.isNull()) {
    resp["id"] = idVar;
  }

  if (!act) {
    resp["ok"] = false;
    JsonObject e = resp["e"].to<JsonObject>();
    e["code"] = "ENOACT";
    e["msg"] = "missing \"act\"";
    return false;
  }

  // Who is asking. The transport decides; modules — not transports — decide
  // what a level buys (see mod_storage.cpp's requireAuth).
  CmdContext ctx;
  ctx.transport = (transport != nullptr) ? transport : "?";
  ctx.authLevel = authLevel;
  ctx.reqId = idVar.is<uint32_t>() ? idVar.as<uint32_t>() : 0;

  // A request carrying "mod" is aimed at a module, not at a built-in command.
  // The registry answers ENOMOD / EDISABLED / EREBOOT (all naming the module)
  // before the module's own dispatch is ever reached.
  const char *mod = req["mod"] | (const char *)nullptr;
  const Command *cmd = nullptr;
  if (mod == nullptr) {
    // ---- THE AUTH GATE (backlog S1) -------------------------------------
    //
    // ONE check, before any handler runs, rather than thirteen inside them.
    // The level per command is CmdAuth::BUILTINS (cmdauth.h); nothing in this
    // file decides policy, and no built-in reads ctx.authLevel for itself.
    //
    // It is what lets a transport dispatch honestly: mod_http.cpp's 401 is now
    // defence in depth, and the BLE adapter may hand an unauthenticated client
    // straight to this function.
    //
    // Order matters. The PRE-FLIGHT check comes before findCommand() so a
    // caller below the lowest level any built-in accepts is refused without
    // being told whether the name it sent exists — otherwise EUNKNOWN vs EAUTH
    // enumerates the whole built-in surface for a client with no session.
    if (!CmdAuth::permits(authLevel, CmdAuth::minimumLevel())) {
      return authDenied(resp, "any built-in command", CmdAuth::minimumLevel(), ctx);
    }
    cmd = findCommand(act);
    if (!cmd) {
      resp["ok"] = false;
      JsonObject e = resp["e"].to<JsonObject>();
      e["code"] = "EUNKNOWN";
      e["msg"] = String("unknown command: ") + act;
      return false;
    }
    if (!CmdAuth::permits(authLevel, cmd->minAuth)) {
      char what[48];
      snprintf(what, sizeof(what), "the built-in command '%.16s'", cmd->name);
      return authDenied(resp, what, cmd->minAuth, ctx);
    }
  }

  // Both `e` and `d` are created up front and the unused one removed, so a
  // failing handler can attach machine-readable detail to `e` (see the
  // CommandHandler contract above). "ok" is inserted first, and `e` before
  // `d`, purely to keep the wire order that ARCHITECTURE.md section 2 shows —
  // {"id","ok","d"} on success, {"id","ok","e","d"} on failure. ArduinoJson
  // emits members in insertion order.
  resp["ok"] = false;
  JsonObject err = resp["e"].to<JsonObject>();
  JsonObject data = resp["d"].to<JsonObject>();
  // Placeholders, for the same insertion-order reason: an error object always
  // reads {"code","msg",...} with any handler-supplied extras after, never
  // with code/msg buried at the end.
  err["code"] = "EFAIL";
  err["msg"] = "command failed";

  CmdError cerr;
  cerr.code = nullptr;
  cerr.msg[0] = '\0';
  DispatchResult res = (mod != nullptr) ? registry.dispatch(mod, act, ctx, params, data, &cerr)
                                        : cmd->handler(ctx, params, data, err, &cerr);

  if (res == DISPATCH_FAIL) {
    // Fill `e` before removing anything, so nothing is added to the document
    // between a removal and a read of the handle it might have recycled.
    err["code"] = cerr.code ? cerr.code : "EFAIL";
    err["msg"] = cerr.msg[0] ? cerr.msg : "command failed";
    resp["ok"] = false;
    // KEEP a non-empty `d` on an error: partial results and diagnostic counts
    // are exactly what a failure needs to carry. Only an empty `d` is dropped,
    // and only to keep the common error response tight.
    if (data.size() == 0) {
      resp.remove("d");
    }
  } else {
    resp.remove("e");
    resp["ok"] = true;
    // ACCEPTED: the work was queued and `d` describes the job. The completion
    // arrives later as an event carrying the same id (see bus.h). Reported
    // separately from OK so a caller knows to wait for it.
    if (res == DISPATCH_ACCEPTED) {
      resp["accepted"] = true;
    }
  }

  // `mod == nullptr` matters: without it, a future module with a "reboot"
  // action would restart the chip as a side effect of its own command. The
  // restart itself belongs to the transport — only it knows how to flush.
  return res == DISPATCH_OK && mod == nullptr && strcmp(act, "reboot") == 0;
}

void fillModules(JsonObject d) {
  registry.list(d["modules"].to<JsonArray>());

  // What the persisted enable-set did at boot. A persisted combination that
  // is no longer satisfiable is skipped rather than fatal, so this is the
  // only place it surfaces.
  const ModuleRestoreReport &rr = registry.restoreReport();
  JsonObject boot = d["boot"].to<JsonObject>();
  boot["nvs"] = rr.nvsRead ? "read" : "empty";
  boot["nvs_write_ok"] = rr.nvsWriteOk;
  // The KNOWN-module set (modset.h). "empty" means this boot could not tell a
  // newly added module from one the owner disabled, so it assumed the latter —
  // which is why a default-on module added by a firmware update comes up on
  // the boot AFTER the one that first wrote this key.
  boot["known"] = rr.knownRead ? "read" : "empty";
  if (rr.nvsTooLong) {
    // Distinct from "nothing is enabled", which is what this used to look
    // like: the stored string did not fit the read buffer, so NOTHING was
    // restored and the next explicit enable/disable will overwrite it.
    boot["nvs_too_long"] = true;
    boot["nvs_stored_len"] = rr.nvsStoredLen;
  }
  JsonArray restored = boot["restored"].to<JsonArray>();
  for (uint8_t i = 0; i < rr.restoredCount; i++) {
    restored.add(rr.restored[i]);
  }
  JsonArray skipped = boot["skipped"].to<JsonArray>();
  for (uint8_t i = 0; i < rr.skippedCount; i++) {
    skipped.add(rr.skipped[i]);
  }
  JsonArray unknown = boot["unknown"].to<JsonArray>();
  for (uint8_t i = 0; i < rr.unknownCount; i++) {
    unknown.add(rr.unknown[i]);
  }
  JsonArray armed = boot["armed"].to<JsonArray>();
  for (uint8_t i = 0; i < rr.armedCount; i++) {
    armed.add(rr.armed[i]);
  }
  // Modules this device had never seen before, which therefore took their
  // descriptor's defaultEnabled rather than being treated as "off".
  JsonArray defaulted = boot["defaulted"].to<JsonArray>();
  for (uint8_t i = 0; i < rr.defaultedCount; i++) {
    defaulted.add(rr.defaulted[i]);
  }
}

void poll() {
  int avail = Serial.available();
  for (int i = 0; i < avail; i++) {
    int ci = Serial.read();
    if (ci < 0) {
      break;
    }
    char c = (char)ci;

    if (c == '\n' || c == '\r') {
      if (lineOverflowed) {
        sendErrorNoId("ELINE", "line too long, discarded");
        lineOverflowed = false;
        lineLen = 0;
      } else if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        processLine(lineBuf);
        lineLen = 0;
      }
      continue;
    }

    if (lineOverflowed) {
      continue;  // discard the rest of an over-long line
    }
    if (lineLen >= Protocol::MAX_LINE) {
      lineOverflowed = true;
      continue;
    }
    lineBuf[lineLen++] = c;
  }
}

bool connected() { return (bool)Serial; }

uint32_t requestsAnswered() { return requestsAnswered_.load(std::memory_order_relaxed); }

}  // namespace Console
