#include "console.h"

#include <Arduino.h>
#include <ArduinoJson.h>
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
#include "partition_info.h"
#include "protocol.h"
#include "bootprobe.h"
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
};

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

extern const Command COMMANDS[];
extern const size_t COMMAND_COUNT;

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

DispatchResult cmdHelp(const CmdContext &, JsonObjectConst, JsonObject d, JsonObject, CmdError *) {
  JsonArray cmds = d["commands"].to<JsonArray>();
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    JsonObject o = cmds.add<JsonObject>();
    o["act"] = COMMANDS[i].name;
    o["help"] = COMMANDS[i].help;
  }
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

const Command COMMANDS[] = {
    {"help", "list commands", cmdHelp},
    {"info", "chip/build/partition identity", cmdInfo},
    {"parts", "live partition table", cmdParts},
    {"mem", "heap free / min-free / largest block", cmdMem},
    {"uptime", "ms since boot", cmdUptime},
    {"tasks", "scheduler task table (intervals, worst-case runtimes)", cmdTasks},
    {"led", "alias for mod:\"led\" act:\"set\"; p:{rgb:\"rrggbb\"} or p:{rgb:\"off\"}", cmdLed},
    {"modules", "registered modules: id, name, category, enabled, claims, actions, blocked_by, status", cmdModules},
    {"enable", "enable a module; p:{id:\"led\"[,force:true]}", cmdEnable},
    {"disable", "disable a module; p:{id:\"led\"}", cmdDisable},
    {"selftest", "assert the claim arbitration rule against a synthetic case table", cmdSelftest},
    {"log", "get/set runtime log level; p:{level:\"error|warn|info|debug|trace\"}", cmdLog},
    {"bootprobe", "what the static-ctor NVS probe saw (hid arming depends on it)", cmdBootProbe},
    {"reboot", "esp_restart() — responds first", cmdReboot},
};
const size_t COMMAND_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

const Command *findCommand(const char *name) {
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    if (strcmp(COMMANDS[i].name, name) == 0) {
      return &COMMANDS[i];
    }
  }
  return nullptr;
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
    cmd = findCommand(act);
    if (!cmd) {
      resp["ok"] = false;
      JsonObject e = resp["e"].to<JsonObject>();
      e["code"] = "EUNKNOWN";
      e["msg"] = String("unknown command: ") + act;
      return false;
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

}  // namespace Console
