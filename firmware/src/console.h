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

#include <ArduinoJson.h>
#include <stdint.h>

namespace Console {

// Call once from setup(), after Serial.begin(). Registers this transport's
// event sink with the bus.
void begin();

// ---- the shared request -> response core (W2) ---------------------------
//
// Executes ONE protocol request and BUILDS the response object into `resp`.
// Sends nothing, touches no transport, and does not restart the chip.
//
// This exists because W2's HTTP transport has to serve the SAME command
// surface at `POST /api/cmd` as this console serves over CDC. The alternative —
// a second dispatcher in mod_http.cpp — means two command tables, two error
// vocabularies, and a REST mirror that drifts from the console the first time
// a built-in is added. It lives in console.h rather than a new file only
// because the built-in command table lives in console.cpp; nothing about it is
// CDC-specific.
//
// `authLevel` is an AuthLevel (registry.h) and `transport` is the name that
// reaches CmdContext — "cdc" (AUTH_PHYSICAL), "http"/"ws" (AUTH_TOKEN).
//
// THE BUILT-INS NOW GATE THEMSELVES (backlog S1, 2026-08-17). Every command in
// the table carries a minimum AuthLevel from CmdAuth::BUILTINS (cmdauth.h) —
// `reboot` at AUTH_PHYSICAL, everything else at AUTH_TOKEN, nothing at
// AUTH_NONE — and this function enforces it centrally before any handler runs.
// Modules continue to gate themselves as before.
//
// So A TRANSPORT MAY DISPATCH AT AUTH_NONE. It will get an EAUTH response
// indistinguishable from a module's, not a hardware detail or a restart. That
// is the property the BLE adapter is meant to rely on; mod_http.cpp's 401 is
// kept as defence in depth rather than as the only defence.
//
// Returns TRUE when the request was a successful built-in `reboot`, meaning
// the caller must flush its transport and then esp_restart(). The restart is
// deliberately the caller's job: only the transport knows how to get the
// response out of the door first.
//
// Thread-safety: re-entrant and stateless apart from the registry and the
// modules it dispatches into, both of which take their own locks. Safe to call
// from the HTTP server task.
bool execute(JsonObjectConst req, uint8_t authLevel, const char *transport, JsonDocument &resp);

// Renders the module listing + boot restore report into `d` — the exact `d` of
// the `modules` command, and the exact body of GET /api/modules. One
// implementation so the two cannot disagree.
//
// `authLevel` is the CALLER's AuthLevel and it FILTERS the result (backlog S7):
// a module whose minAuth this level does not clear is omitted entirely, so a
// listing cannot be used to enumerate a device the caller has no session on.
// It also decides the per-action `allowed` flag, and is echoed back as
// `auth`/`auth_level` so a UI knows why something is greyed out.
void fillModules(JsonObject d, uint8_t authLevel);

// Scheduler task: drains whatever's waiting on Serial, non-blocking, and
// dispatches any complete line(s). Register with interval 0 (every pass) for
// responsiveness — this only does work when bytes are actually waiting.
void poll();

// True while the host has the CDC port open (reported by the `cdc` module's
// status). Always true on builds where the framework cannot tell.
bool connected();

// How many protocol requests execute() has answered since boot, over EVERY
// transport — CDC, HTTP and WebSocket alike. Counts refusals and errors too:
// the question it answers is "did the command core run end to end", not "did
// anyone like the result".
//
// It exists for otahealth.cpp's CRIT_CONSOLE (otadecide.h). Deliberately
// transport-agnostic, for the same reason modules emit on the bus rather than
// naming a transport: an image confirmed over Wi-Fi is as alive as one
// confirmed over the cable.
//
// Thread-safety: a relaxed std::atomic, incremented on whichever task
// dispatched (the loop task or esp_http_server's). It never decreases and no
// other state is published through it.
uint32_t requestsAnswered();

}  // namespace Console
