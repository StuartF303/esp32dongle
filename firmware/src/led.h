// Single onboard APA102 RGB LED (data GPIO 40, clock GPIO 39). Bit-banged: at
// one LED, pulling in FastLED/Adafruit_DotStar for this is not worth the
// flash budget.
//
// This is now a plain driver and nothing else. Ownership of the LED — who is
// allowed to drive it, and whether the heartbeat runs — belongs to the `led`
// module (mod_led.h), which claims Claims::RES_LED exclusively from the
// registry. Nothing else in the image should call into here directly.
//
// Two owners still share the LED once the module is enabled: a slow amber
// heartbeat blink (liveness) and the module's `set` action (explicit user
// colour). `set` takes manual control; clearOverride() hands it back, and
// enabling the module does that for you — so disable+enable is the "back to
// auto" path that used to be missing.

#pragma once

#include <stdint.h>

namespace Led {

// Configures the GPIOs and drives the LED off.
void begin();

// Manual colour. Suppresses the heartbeat blink until clearOverride().
void setOverrideColor(uint8_t r, uint8_t g, uint8_t b);

// Back to heartbeat mode.
void clearOverride();

bool overrideActive();

// Drives the LED dark without touching the override flag. Used when the `led`
// module is disabled: the module has given up the resource, so it must leave
// the hardware quiet rather than mid-blink.
void off();

// Most recent colour written, for the module's status().
void currentColor(uint8_t *r, uint8_t *g, uint8_t *b);

// One step of the amber liveness blink (register at ~500 ms). No-op while an
// override colour is set. Call this from the module's task, not directly from
// setup() — see mod_led.h.
void heartbeatTask();

}  // namespace Led
