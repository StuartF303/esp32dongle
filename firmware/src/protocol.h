// usbdongle W1 — wire protocol constants shared by every transport.
//
// ONE definition of the framing limits, deliberately. W2 adds a WebSocket and
// a BLE GATT adapter; if each of them picks its own maximum line length then a
// request that works over CDC silently truncates over WS, and the failure
// shows up as a JSON parse error at the far end with no clue why. Everything
// that frames a line — console.cpp today, ws/ble tomorrow — reads it here.
//
// The protocol itself (request/response/event shapes) is ARCHITECTURE.md
// section 2.

#pragma once

#include <stddef.h>

namespace Protocol {

// Longest single request line accepted, in bytes, EXCLUDING the terminator.
// A line longer than this is discarded with ELINE rather than silently cut.
//
// 1024, not 256: `hid.type` carries its payload as p.text, and a macro line of
// a couple of hundred characters plus JSON escaping goes past 256 routinely.
// Cost is one static buffer per transport, which is why the number lives in
// one place — see the RAM note in registry.h.
constexpr size_t MAX_LINE = 1024;

}  // namespace Protocol
