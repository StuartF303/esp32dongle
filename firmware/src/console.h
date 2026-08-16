// usbdongle W1 — command console over USB CDC.
//
// Line-delimited JSON per ARCHITECTURE.md section 2:
//   request:  {"id":7,"act":"info"}                (optional "mod", optional "p" params object)
//   response: {"id":7,"ok":true,"d":{...}}          or {"id":7,"ok":false,"e":{"code":"...","msg":"..."},"d":{...}}
//   event:    {"ev":"...","d":{...}}
//
// `d` now survives an error response when it is non-empty: a partial result
// (a truncated scan, a listing with unreadable entries, a self-test that ran
// fine and found failures) is data the caller needs, and throwing it away to
// keep the shape tidy made every such command choose between reporting the
// failure and reporting the numbers.
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
//
// This is a TRANSPORT. It supplies the CmdContext (transport "cdc",
// AUTH_PHYSICAL — a cable is consent) and registers itself as an event sink on
// the bus (bus.h). Modules never call into here; see the `cdc` module
// descriptor in mod_cdc.h for how the console appears in the registry.

#pragma once

namespace Console {

// Call once from setup(), after Serial.begin(). Registers this transport's
// event sink with the bus.
void begin();

// Scheduler task: drains whatever's waiting on Serial, non-blocking, and
// dispatches any complete line(s). Register with interval 0 (every pass) for
// responsiveness — this only does work when bytes are actually waiting.
void poll();

// True while the host has the CDC port open (reported by the `cdc` module's
// status). Always true on builds where the framework cannot tell.
bool connected();

}  // namespace Console
