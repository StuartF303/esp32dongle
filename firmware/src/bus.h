// usbdongle W1 — transport-agnostic event bus.
//
// WHY THIS EXISTS: ARCHITECTURE.md section 2 says "a module never knows which
// transport a command arrived on". A module that calls Console::sendEvent() to
// report a scan result has just hard-wired itself to USB CDC — the same event
// then does NOT reach the phone over WebSocket, and the only fix is to edit
// every module once W2 lands. So modules emit here, and transports subscribe.
//
// The bus does no formatting, no queueing and no allocation of its own: it
// hands (name, fill, ctx) to each registered sink and lets the sink serialise
// into whatever framing it uses. Emitting therefore costs one indirect call
// per sink plus whatever that sink does.
//
// ---- THREADING (revised for W2's HTTP transport) ------------------------
//
// Until W2 there was one task in the system and this was a non-question. There
// are now two: the Arduino loop task (priority 1) and esp_http_server's own
// task (priority 5). A command arriving over HTTP is dispatched on the HTTP
// task, so a module can emit() from EITHER, concurrently.
//
// What is guaranteed here:
//   * emit() is safe to call from any task, concurrently with other emit()s
//     and with addSink(). The table is append-only and a sink pointer is
//     published (release) before the count that exposes it is incremented
//     (acquire on the read side), so a sink is never called half-registered.
//   * addSink() is safe against a concurrent addSink() — it takes a spinlock —
//     and is idempotent per sink pointer.
//
// What is NOT guaranteed, and callers must handle:
//   * A SINK MAY RUN ON TWO TASKS AT ONCE. Nothing here serialises sinks. A
//     sink that writes to a shared byte stream must lock internally (console.cpp
//     does exactly that, or two JSON lines interleave on the CDC console and
//     both become unparseable).
//   * A sink is never REMOVED — there is no removeSink(), deliberately, because
//     removal would need to be safe against an emit() already in flight on
//     another core. A transport that can be disabled therefore keeps its sink
//     registered for the life of the image and makes the sink itself a no-op
//     while it is down (mod_http.cpp).
//   * Ordering between events emitted from different tasks is whatever the
//     scheduler does. Events carrying a sequence have to carry it themselves.
//   * A sink runs on the EMITTER's task, with whatever locks the emitter holds.
//     Modules emit from inside a registry-locked dispatch, so a sink must not
//     block for long and must not call back into the registry.

#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

namespace Bus {

// Fills the event's `d` object. `ctx` is passed through untouched from
// emit().
//
// THE ctx POINTER IS NOT OPTIONAL. `wifiscan` emits one event per AP found and
// has to get that AP into fill() somehow; without ctx the only way is a file-
// scope variable holding "the AP currently being emitted", which is a global
// that exists purely to work around the callback signature. With ctx the AP is
// passed as an argument and fill() stays reentrant.
typedef void (*FillFn)(JsonObject d, void *ctx);

// A transport's event output. Receives exactly what emit() was given and is
// responsible for calling fill() (or not — a sink with no client attached may
// legitimately drop the event without ever building the JSON).
typedef void (*SinkFn)(const char *name, FillFn fill, void *ctx);

// Room for CDC + WS + BLE + one spare. Fixed size, no allocation.
constexpr uint8_t MAX_SINKS = 4;

// Registers a transport's event output. Call from that transport's begin().
// Returns false if the table is full or `sink` is null; duplicates are
// rejected, so a double begin() cannot double every event.
bool addSink(SinkFn sink);

// Publishes an event to every registered sink. `fill` may be nullptr for an
// event with no data; `ctx` may be nullptr.
//
// Emitting with no sinks registered is a silent no-op by design: a module must
// work identically whether or not anyone is listening.
void emit(const char *name, FillFn fill, void *ctx);

uint8_t sinkCount();

}  // namespace Bus
