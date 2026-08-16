#include "mod_led.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "led.h"

namespace {

// NOTE: no local `moduleEnabled` mirror any more. The registry owns the tick
// (ModuleDescriptor::tick / tickIntervalMs) and only calls it while the module
// is enabled, so there is exactly one source of truth for "is this on" —
// Registry::enabled_[i] — instead of two that could disagree after a failed
// enable().

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
  return true;
}

bool ledDisable(const char **errMsg) {
  (void)errMsg;
  Led::off();  // give the resource up dark, not mid-blink
  return true;
}

DispatchResult ledDispatch(const CmdContext &ctx, const char *act, JsonObjectConst p, JsonObject d, CmdError *err) {
  (void)ctx;  // the LED is harmless from any transport at any auth level

  if (strcmp(act, "set") == 0) {
    const char *rgb = p["rgb"] | (const char *)nullptr;
    if (rgb == nullptr) {
      cmdErrorf(err, "EARGS", "missing p.rgb (6 hex digits, e.g. \"ff0000\", or \"off\")");
      return DISPATCH_FAIL;
    }

    uint8_t r = 0, g = 0, b = 0;
    if (strcasecmp(rgb, "off") != 0 && !parseHexColor(rgb, &r, &g, &b)) {
      cmdErrorf(err, "EARGS", "rgb must be 6 hex digits, e.g. \"ff0000\", or \"off\" (got \"%.16s\")", rgb);
      return DISPATCH_FAIL;
    }

    Led::setOverrideColor(r, g, b);
    char buf[7];
    snprintf(buf, sizeof(buf), "%02x%02x%02x", r, g, b);
    d["mode"] = "manual";
    d["rgb"] = buf;
    return DISPATCH_OK;
  }

  if (strcmp(act, "auto") == 0) {
    Led::clearOverride();
    d["mode"] = "heartbeat";
    return DISPATCH_OK;
  }

  cmdErrorf(err, "EUNKNOWN", "unknown action for module 'led': \"%.16s\" (try \"set\" or \"auto\")", act);
  return DISPATCH_FAIL;
}

void ledStatus(JsonObject d) {
  uint8_t r = 0, g = 0, b = 0;
  Led::currentColor(&r, &g, &b);
  char buf[7];
  snprintf(buf, sizeof(buf), "%02x%02x%02x", r, g, b);
  d["mode"] = Led::overrideActive() ? "manual" : "heartbeat";
  d["rgb"] = buf;
}

// Static, .rodata. This is what lets a UI render the module's controls
// without a line of module-specific front-end code — see ARCHITECTURE.md
// section 2.
const ModuleAction LED_ACTIONS[] = {
    {"set", "set a fixed colour", "rgb:\"rrggbb\"|\"off\""},
    {"auto", "return to the heartbeat blink", ""},
};

void ledTick() { Led::heartbeatTask(); }

const ModuleDescriptor LED_MODULE = {
    .id = "led",
    .name = "Status LED",
    .category = "status",
    .claims = Claims::claim(Claims::RES_LED, Claims::CLAIM_EXCLUSIVE),
    // On out of the box. Declared here rather than as a hardcoded id in
    // main.cpp, so "what runs on a virgin device" is answered in the module's
    // own file.
    .defaultEnabled = true,
    .bootTimeBinding = false,
    .essential = false,
    .enable = ledEnable,
    .disable = ledDisable,
    .dispatch = ledDispatch,
    .status = ledStatus,
    .actions = LED_ACTIONS,
    .actionCount = (uint8_t)(sizeof(LED_ACTIONS) / sizeof(LED_ACTIONS[0])),
    // The registry registers this with the scheduler and gates it on the
    // enabled flag; the module neither registers nor gates it itself.
    .tick = ledTick,
    .tickIntervalMs = 500,
};

}  // namespace

const ModuleDescriptor *ledModuleDescriptor() { return &LED_MODULE; }
