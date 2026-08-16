// usbdongle W1 — command console over USB CDC.
//
// Line-delimited JSON per ARCHITECTURE.md section 2:
//   request:  {"id":7,"act":"info"}                (optional "mod", optional "p" params object)
//   response: {"id":7,"ok":true,"d":{...}}          or {"id":7,"ok":false,"e":{"code":"...","msg":"..."}}
//   event:    {"ev":"...","d":{...}}
//
// Also accepts a bare word as shorthand, so the console is typeable by hand:
//   info             -> {"act":"info"}
//   led ff0000       -> {"act":"led","p":{"rgb":"ff0000"}}
//   log debug        -> {"act":"log","p":{"level":"debug"}}
//   enable led       -> {"act":"enable","p":{"id":"led"}}
//   enable msc force -> {"act":"enable","p":{"id":"msc","force":true}}
//   <anything else>  -> {"act":"<word>","p":{"arg":"<rest>"}}
//
// Two kinds of command reach here:
//   * built-ins (help/info/parts/mem/uptime/tasks/log/reboot, plus
//     modules/enable/disable/selftest) — the table in console.cpp;
//   * anything carrying "mod", which is handed straight to the module
//     registry (registry.h). The console does no module-specific work.

#pragma once

#include <ArduinoJson.h>

namespace Console {

// Call once from setup(), after Serial.begin().
void begin();

// Scheduler task: drains whatever's waiting on Serial, non-blocking, and
// dispatches any complete line(s). Register with interval 0 (every pass) for
// responsiveness — this only does work when bytes are actually waiting.
void poll();

// Sends an unsolicited event line: {"ev":"<name>","d":{...}}. `fill` is
// called with the (empty) "d" object to populate; pass nullptr for an event
// with no data.
void sendEvent(const char *name, void (*fill)(JsonObject d));

}  // namespace Console
