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
// Threading: sinks are registered once at boot (Console::begin() and friends)
// and the table is never mutated afterwards, so reads need no lock. emit()
// itself is only as thread-safe as the sinks are — a sink writing to Serial
// from two tasks at once is the sink's problem, not the bus's.

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
