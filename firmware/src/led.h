// Single onboard APA102 RGB LED (data GPIO 40, clock GPIO 39). Bit-banged: at
// one LED, pulling in FastLED/Adafruit_DotStar for this is not worth the
// flash budget.
//
// Two owners share this one LED: a slow amber heartbeat blink (liveness,
// runs from boot) and the console's `led` command (explicit user colour).
// Once `led` has been used, it has permanent manual control until reboot —
// there is no "back to auto" command, because none was asked for. If you
// want one, say so.

#pragma once

#include <stdint.h>

namespace Led {

// Configures the GPIOs and drives the LED off.
void begin();

// Console `led` command: takes manual control of the LED and shows this
// colour. Suppresses the heartbeat blink from here on (until reboot).
void setOverrideColor(uint8_t r, uint8_t g, uint8_t b);

// Scheduler task (register at ~500 ms): amber liveness blink. No-op once
// setOverrideColor() has been called.
void heartbeatTask();

}  // namespace Led
