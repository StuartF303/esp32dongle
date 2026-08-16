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
// 4096, raised from 1024 on 2026-08-16 for `storage`. This is a THROUGHPUT
// number, not a comfort one. File payloads travel base64-encoded, which inflates
// 4/3, so at 1024 a `storage.read` response had room for roughly 675 raw bytes
// once the JSON envelope was paid for — about 50 KB/s over CDC at one round trip
// per chunk, and worse over BLE where the round trip is dearer. 4096 gives
// ~2 KB raw chunks (storage.caps advertises max_chunk = 2048), which is the
// point at which the per-request overhead stops dominating.
//
// It stays ONE number across transports. BLE fragments far below this at the
// link layer (ATT_MTU is 23-517 bytes) and the WS adapter will frame whole
// lines, but both reassemble to the same logical line, so a request that works
// over CDC must work unchanged over BLE. A per-transport limit is exactly the
// silent-truncation bug this file exists to prevent.
//
// Cost is one static buffer per transport: console.cpp's lineBuf goes from
// 1025 to 4097 bytes, +3072 B of static RAM per transport that frames lines.
// Budget accordingly when W2 adds WS and BLE — see the RAM note in registry.h.
//
// It is NOT a response-size limit for anything but sanity: modules bound their
// own output (storage.list pages, storage.read caps at max_chunk) so that a
// response fits inside this with the envelope, and callers negotiate from
// storage.caps rather than guessing.
constexpr size_t MAX_LINE = 4096;

}  // namespace Protocol
