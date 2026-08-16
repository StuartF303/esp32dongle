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

#include "led.h"
#include "partition_info.h"
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
typedef bool (*CommandHandler)(JsonObjectConst p, JsonObject d, const char **errCode, const char **errMsg);

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

bool parseHexColor(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
  if (s[0] == '#') {
    s++;
  }
  if (strlen(s) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    if (!isxdigit((unsigned char)s[i])) {
      return false;
    }
  }
  char byte[3] = {0, 0, 0};
  byte[0] = s[0];
  byte[1] = s[1];
  *r = (uint8_t)strtol(byte, nullptr, 16);
  byte[0] = s[2];
  byte[1] = s[3];
  *g = (uint8_t)strtol(byte, nullptr, 16);
  byte[0] = s[4];
  byte[1] = s[5];
  *b = (uint8_t)strtol(byte, nullptr, 16);
  return true;
}

// ---- handlers --------------------------------------------------------

extern const Command COMMANDS[];
extern const size_t COMMAND_COUNT;

bool cmdHelp(JsonObjectConst, JsonObject d, const char **, const char **) {
  JsonArray cmds = d["commands"].to<JsonArray>();
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    JsonObject o = cmds.add<JsonObject>();
    o["act"] = COMMANDS[i].name;
    o["help"] = COMMANDS[i].help;
  }
  return true;
}

bool cmdInfo(JsonObjectConst, JsonObject d, const char **, const char **) {
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

bool cmdParts(JsonObjectConst, JsonObject d, const char **, const char **) {
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

bool cmdMem(JsonObjectConst, JsonObject d, const char **, const char **) {
  d["free_heap"] = ESP.getFreeHeap();
  d["min_free_heap"] = ESP.getMinFreeHeap();
  d["largest_free_block"] = ESP.getMaxAllocHeap();
  return true;
}

bool cmdUptime(JsonObjectConst, JsonObject d, const char **, const char **) {
  d["uptime_ms"] = millis();
  return true;
}

bool cmdTasks(JsonObjectConst, JsonObject d, const char **, const char **) {
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

bool cmdLed(JsonObjectConst p, JsonObject d, const char **errCode, const char **errMsg) {
  const char *rgb = p["rgb"] | (const char *)nullptr;
  if (!rgb) {
    *errCode = "EARGS";
    *errMsg = "missing p.rgb (6 hex digits, e.g. \"ff0000\", or \"off\")";
    return false;
  }

  uint8_t r = 0, g = 0, b = 0;
  if (strcasecmp(rgb, "off") != 0) {
    if (!parseHexColor(rgb, &r, &g, &b)) {
      *errCode = "EARGS";
      *errMsg = "rgb must be 6 hex digits, e.g. \"ff0000\", or \"off\"";
      return false;
    }
  }

  Led::setOverrideColor(r, g, b);

  char buf[7];
  snprintf(buf, sizeof(buf), "%02x%02x%02x", r, g, b);
  d["rgb"] = buf;
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
bool cmdLog(JsonObjectConst p, JsonObject d, const char **errCode, const char **errMsg) {
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

bool cmdReboot(JsonObjectConst, JsonObject d, const char **, const char **) {
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
    {"led", "p:{rgb:\"rrggbb\"} or p:{rgb:\"off\"}", cmdLed},
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

  const Command *cmd = findCommand(act);
  if (!cmd) {
    resp["ok"] = false;
    JsonObject e = resp["e"].to<JsonObject>();
    e["code"] = "EUNKNOWN";
    e["msg"] = String("unknown command: ") + act;
    sendLine(resp);
    return;
  }

  JsonObject data = resp["d"].to<JsonObject>();
  const char *errCode = nullptr;
  const char *errMsg = nullptr;
  bool ok = cmd->handler(params, data, &errCode, &errMsg);

  if (ok) {
    resp["ok"] = true;
  } else {
    resp.remove("d");
    resp["ok"] = false;
    JsonObject e = resp["e"].to<JsonObject>();
    e["code"] = errCode ? errCode : "EFAIL";
    e["msg"] = errMsg ? errMsg : "command failed";
  }

  sendLine(resp);

  if (ok && strcmp(act, "reboot") == 0) {
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
  const char *rest = nullptr;

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
    const char *key = strcmp(act, "led") == 0 ? "rgb" : (strcmp(act, "log") == 0 ? "level" : "arg");
    reqDoc["p"][key] = rest;
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
