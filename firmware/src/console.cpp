#include "console.h"

#include <Arduino.h>
#include <ctype.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <stdlib.h>
#include <string.h>

#include "claims_selftest.h"
#include "partition_info.h"
#include "registry.h"
#include "scheduler.h"

namespace {

// ---- line reader: non-blocking, bounded --------------------------------

const size_t LINE_BUF_SIZE = 256;
char lineBuf[LINE_BUF_SIZE];
size_t lineLen = 0;
bool lineOverflowed = false;

// ---- command dispatch ----------------------------------------------------

// Handler contract: read `p` (may be a null/empty object if the request had
// no "p"), fill `d` on success and return true, or set *errCode/*errMsg and
// return false. Never blocks except `reboot`, which is a deliberate,
// documented one-off (see dispatch()).
//
// `e` is the error object. It is pre-created and thrown away on success, so a
// handler that returns false can attach machine-readable detail alongside the
// {code,msg} pair from ARCHITECTURE.md section 2 — `enable` uses it for
// "blocked_by"/"stopped", because a UI cannot parse those out of prose.
typedef bool (*CommandHandler)(JsonObjectConst p, JsonObject d, JsonObject e, const char **errCode,
                               const char **errMsg);

struct Command {
  const char *name;
  const char *help;
  CommandHandler handler;
};

void sendLine(JsonDocument &doc) {
  serializeJson(doc, Serial);
  Serial.print('\n');
}

void sendErrorNoId(const char *code, const char *msg) {
  JsonDocument resp;
  resp["ok"] = false;
  JsonObject e = resp["e"].to<JsonObject>();
  e["code"] = code;
  e["msg"] = msg;
  sendLine(resp);
}

// ---- handlers --------------------------------------------------------

extern const Command COMMANDS[];
extern const size_t COMMAND_COUNT;

bool cmdHelp(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
  JsonArray cmds = d["commands"].to<JsonArray>();
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    JsonObject o = cmds.add<JsonObject>();
    o["act"] = COMMANDS[i].name;
    o["help"] = COMMANDS[i].help;
  }
  return true;
}

bool cmdInfo(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
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

  return true;
}

bool cmdParts(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
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

  return true;
}

bool cmdMem(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
  d["free_heap"] = ESP.getFreeHeap();
  d["min_free_heap"] = ESP.getMinFreeHeap();
  d["largest_free_block"] = ESP.getMaxAllocHeap();
  return true;
}

bool cmdUptime(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
  d["uptime_ms"] = millis();
  return true;
}

bool cmdTasks(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
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
  return true;
}

// `led` is now a convenience alias for {"mod":"led","act":"set"} and owns no
// state of its own — the LED belongs to the `led` module, which claims
// Claims::RES_LED exclusively. Disabling that module makes this command fail
// with EDISABLED, which is the point of the exercise.
bool cmdLed(JsonObjectConst p, JsonObject d, JsonObject, const char **errCode, const char **errMsg) {
  return registry.dispatch("led", "set", p, d, errCode, errMsg);
}

// ---- module registry commands -------------------------------------------

// Holds the message built by Registry::enable()/disable() for as long as it
// takes dispatch() to serialise the response. ArduinoJson stores a
// `const char *` by reference rather than copying, and ModuleActionResult
// lives on the handler's stack, so the text has to be somewhere that outlives
// the handler. Single-threaded loop task, one command at a time - see the
// threading note in registry.h.
char actionMsg[sizeof(ModuleActionResult::msg)];

bool renderActionResult(const char *id, const ModuleActionResult &r, JsonObject d, JsonObject e, const char **errCode,
                        const char **errMsg) {
  snprintf(actionMsg, sizeof(actionMsg), "%s", r.msg);

  if (r.ok) {
    d["id"] = id;
    d["enabled"] = r.enabledAfter;
    d["changed"] = r.changed;  // false == it was already in that state
    d["msg"] = actionMsg;
    JsonArray stopped = d["stopped"].to<JsonArray>();
    for (uint8_t i = 0; i < r.stoppedCount; i++) {
      stopped.add(r.stopped[i]);
    }
    return true;
  }

  *errCode = r.code != nullptr ? r.code : "EFAIL";
  *errMsg = actionMsg;
  e["id"] = id;
  e["enabled"] = r.enabledAfter;
  if (r.blockedByCount > 0) {
    JsonArray blocked = e["blocked_by"].to<JsonArray>();
    for (uint8_t i = 0; i < r.blockedByCount; i++) {
      blocked.add(r.blockedBy[i]);
    }
  }
  if (r.stoppedCount > 0) {
    // Only ever non-empty on a failed force: these modules ARE stopped.
    JsonArray stopped = e["stopped"].to<JsonArray>();
    for (uint8_t i = 0; i < r.stoppedCount; i++) {
      stopped.add(r.stopped[i]);
    }
  }
  return false;
}

// The exact shape GET /api/modules will serve.
bool cmdModules(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
  registry.list(d["modules"].to<JsonArray>());

  // What the persisted enable-set did at boot. A persisted combination that
  // is no longer satisfiable is skipped rather than fatal, so this is the
  // only place it surfaces.
  const ModuleRestoreReport &rr = registry.restoreReport();
  JsonObject boot = d["boot"].to<JsonObject>();
  boot["nvs"] = rr.nvsRead ? "read" : "empty";
  boot["nvs_write_ok"] = rr.nvsWriteOk;
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
  return true;
}

bool cmdEnable(JsonObjectConst p, JsonObject d, JsonObject e, const char **errCode, const char **errMsg) {
  const char *id = p["id"] | (const char *)nullptr;
  if (id == nullptr) {
    *errCode = "EARGS";
    *errMsg = "missing p.id (module id, see the `modules` command)";
    return false;
  }
  bool force = p["force"] | false;

  ModuleActionResult r;
  registry.enable(id, force, r);
  return renderActionResult(id, r, d, e, errCode, errMsg);
}

bool cmdDisable(JsonObjectConst p, JsonObject d, JsonObject e, const char **errCode, const char **errMsg) {
  const char *id = p["id"] | (const char *)nullptr;
  if (id == nullptr) {
    *errCode = "EARGS";
    *errMsg = "missing p.id (module id, see the `modules` command)";
    return false;
  }

  ModuleActionResult r;
  registry.disable(id, r);
  return renderActionResult(id, r, d, e, errCode, errMsg);
}

// Runs the assertion table in claims_selftest.h against the pure arbitration
// function. Synthetic ClaimSets only — no fake modules are ever registered in
// the live registry, so this cannot perturb real hardware state.
//
// Always ok:true: the command ran. `d.failed` is what to check; a non-zero
// value there means the arbitration rule is broken, not that the command was
// malformed. Keeping it ok:true is what preserves the counts, since dispatch()
// drops `d` on an error response.
bool cmdSelftest(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
  Claims::SelfTestResult res = Claims::runSelfTest();
  d["cases"] = res.total;
  d["passed"] = res.passed;
  d["failed"] = res.failed;
  JsonArray failures = d["failures"].to<JsonArray>();
  for (uint8_t i = 0; i < res.namedFailures; i++) {
    failures.add(res.failures[i]);
  }
  d["failures_truncated"] = res.failed > res.namedFailures;
  return true;
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
bool cmdLog(JsonObjectConst p, JsonObject d, JsonObject, const char **errCode, const char **errMsg) {
  const char *level = p["level"] | (const char *)nullptr;
  if (level) {
    int8_t idx = logLevelIndex(level);
    if (idx < 0) {
      *errCode = "EARGS";
      *errMsg = "level must be one of error/warn/info/debug/trace";
      return false;
    }
    currentLogLevel = (uint8_t)idx;
  }
  d["level"] = LOG_LEVELS[currentLogLevel];
  return true;
}

bool cmdReboot(JsonObjectConst, JsonObject d, JsonObject, const char **, const char **) {
  d["restarting"] = true;
  return true;  // dispatch() actually restarts, after this response is on the wire
}

const Command COMMANDS[] = {
    {"help", "list commands", cmdHelp},
    {"info", "chip/build/partition identity", cmdInfo},
    {"parts", "live partition table", cmdParts},
    {"mem", "heap free / min-free / largest block", cmdMem},
    {"uptime", "ms since boot", cmdUptime},
    {"tasks", "scheduler task table (intervals, worst-case runtimes)", cmdTasks},
    {"led", "alias for mod:\"led\" act:\"set\"; p:{rgb:\"rrggbb\"} or p:{rgb:\"off\"}", cmdLed},
    {"modules", "registered modules: id, name, category, enabled, claims, status", cmdModules},
    {"enable", "enable a module; p:{id:\"led\"[,force:true]}", cmdEnable},
    {"disable", "disable a module; p:{id:\"led\"}", cmdDisable},
    {"selftest", "assert the claim arbitration rule against a synthetic case table", cmdSelftest},
    {"log", "get/set runtime log level; p:{level:\"error|warn|info|debug|trace\"}", cmdLog},
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

void dispatch(JsonObjectConst req) {
  JsonVariantConst idVar = req["id"];
  const char *act = req["act"] | (const char *)nullptr;
  JsonObjectConst params = req["p"];

  JsonDocument resp;
  if (!idVar.isNull()) {
    resp["id"] = idVar;
  }

  if (!act) {
    resp["ok"] = false;
    JsonObject e = resp["e"].to<JsonObject>();
    e["code"] = "ENOACT";
    e["msg"] = "missing \"act\"";
    sendLine(resp);
    return;
  }

  // A request carrying "mod" is aimed at a module, not at a built-in command.
  // The registry answers ENOMOD / EDISABLED (both naming the module) before
  // the module's own dispatch is ever reached.
  const char *mod = req["mod"] | (const char *)nullptr;
  const Command *cmd = nullptr;
  if (mod == nullptr) {
    cmd = findCommand(act);
    if (!cmd) {
      resp["ok"] = false;
      JsonObject e = resp["e"].to<JsonObject>();
      e["code"] = "EUNKNOWN";
      e["msg"] = String("unknown command: ") + act;
      sendLine(resp);
      return;
    }
  }

  // Both `d` and `e` are created up front and the unused one is removed, so a
  // failing handler can attach machine-readable detail to `e` (see the
  // CommandHandler contract above). "ok" is inserted before either of them
  // purely to keep the wire order {"id","ok","d"|"e"} that the protocol
  // examples in ARCHITECTURE.md section 2 show — ArduinoJson emits members in
  // insertion order.
  resp["ok"] = false;
  JsonObject data = resp["d"].to<JsonObject>();
  JsonObject err = resp["e"].to<JsonObject>();
  // Placeholders, for the same insertion-order reason: an error object always
  // reads {"code","msg",...} with any handler-supplied extras after, never
  // with code/msg buried at the end.
  err["code"] = "EFAIL";
  err["msg"] = "command failed";
  const char *errCode = nullptr;
  const char *errMsg = nullptr;
  bool ok = (mod != nullptr) ? registry.dispatch(mod, act, params, data, &errCode, &errMsg)
                             : cmd->handler(params, data, err, &errCode, &errMsg);

  if (ok) {
    resp.remove("e");
    resp["ok"] = true;
  } else {
    // Fill `e` before removing `d`, so nothing is added to the document
    // between a removal and a read of the handle it might have recycled.
    err["code"] = errCode ? errCode : "EFAIL";
    err["msg"] = errMsg ? errMsg : "command failed";
    resp.remove("d");
    resp["ok"] = false;
  }

  sendLine(resp);

  // `mod == nullptr` matters: without it, a future module with a "reboot"
  // action would restart the chip as a side effect of its own command.
  if (ok && mod == nullptr && strcmp(act, "reboot") == 0) {
    // Terminal action, not a scheduler task: the loop is ending for good, so
    // the "never delay() in the main path" rule doesn't apply here. This
    // gives the USB CDC TX queue a moment to actually get the response out
    // before the chip resets.
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
    if (lineLen + 1 >= LINE_BUF_SIZE) {
      lineOverflowed = true;
      continue;
    }
    lineBuf[lineLen++] = c;
  }
}

void sendEvent(const char *name, void (*fill)(JsonObject d)) {
  JsonDocument doc;
  doc["ev"] = name;
  JsonObject d = doc["d"].to<JsonObject>();
  if (fill) {
    fill(d);
  }
  sendLine(doc);
}

}  // namespace Console
