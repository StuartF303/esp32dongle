#include "mod_led.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "led.h"

namespace {

// Mirrors the registry's enabled flag. Kept locally so the 500 ms task costs
// a bool test rather than a strcmp walk of the module table, and so the
// module never has to reach back into the registry that owns it.
bool moduleEnabled = false;

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

bool ledEnable(const char **errMsg) {
  (void)errMsg;
  // Led::begin() already ran in setup(): the GPIOs are configured for the
  // whole life of the image regardless of module state, because two output
  // pins cost nothing and re-running pinMode() on every enable would glitch
  // the LED. What the module owns is *driving* it.
  Led::clearOverride();  // a fresh enable starts in heartbeat mode
  moduleEnabled = true;
  return true;
}

bool ledDisable(const char **errMsg) {
  (void)errMsg;
  moduleEnabled = false;
  Led::off();  // give the resource up dark, not mid-blink
  return true;
}

bool ledDispatch(const char *act, JsonObjectConst p, JsonObject d, const char **errCode, const char **errMsg) {
  if (strcmp(act, "set") == 0) {
    const char *rgb = p["rgb"] | (const char *)nullptr;
    if (rgb == nullptr) {
      *errCode = "EARGS";
      *errMsg = "missing p.rgb (6 hex digits, e.g. \"ff0000\", or \"off\")";
      return false;
    }

    uint8_t r = 0, g = 0, b = 0;
    if (strcasecmp(rgb, "off") != 0 && !parseHexColor(rgb, &r, &g, &b)) {
      *errCode = "EARGS";
      *errMsg = "rgb must be 6 hex digits, e.g. \"ff0000\", or \"off\"";
      return false;
    }

    Led::setOverrideColor(r, g, b);
    char buf[7];
    snprintf(buf, sizeof(buf), "%02x%02x%02x", r, g, b);
    d["mode"] = "manual";
    d["rgb"] = buf;
    return true;
  }

  if (strcmp(act, "auto") == 0) {
    Led::clearOverride();
    d["mode"] = "heartbeat";
    return true;
  }

  *errCode = "EUNKNOWN";
  *errMsg = "unknown action for module 'led' (try \"set\" or \"auto\")";
  return false;
}

void ledStatus(JsonObject d) {
  uint8_t r = 0, g = 0, b = 0;
  Led::currentColor(&r, &g, &b);
  char buf[7];
  snprintf(buf, sizeof(buf), "%02x%02x%02x", r, g, b);
  d["mode"] = Led::overrideActive() ? "manual" : "heartbeat";
  d["rgb"] = buf;
}

const ModuleDescriptor LED_MODULE = {
    "led",
    "Status LED",
    "status",
    Claims::claim(Claims::RES_LED, Claims::CLAIM_EXCLUSIVE),
    ledEnable,
    ledDisable,
    ledDispatch,
    ledStatus,
};

}  // namespace

const ModuleDescriptor *ledModuleDescriptor() { return &LED_MODULE; }

void ledModuleTask() {
  if (!moduleEnabled) {
    return;  // module disabled: the blink stops, and the LED stays dark
  }
  Led::heartbeatTask();
}
