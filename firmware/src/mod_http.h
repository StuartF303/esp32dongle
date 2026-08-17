// usbdongle W2 — the `http` module: Wi-Fi SoftAP + HTTP/WebSocket transport.
//
// The second transport onto the command bus (ARCHITECTURE.md sections 2 and 4).
// It carries the SAME line-delimited JSON protocol as the CDC console — over
// WebSocket text frames, and mirrored request-per-request at POST /api/cmd —
// by calling the one shared dispatcher, Console::execute(). There is no second
// command surface and no HTTP-specific command shape.
//
// ---- WHY THIS IS THE INTERESTING ONE ------------------------------------
//
// It is the first thing in this image that does not run on the Arduino loop
// task. esp_http_server owns a FreeRTOS task at priority 5 (loopTask is 1), and
// every request handler, every WebSocket frame and every queued work item runs
// on it. So this module is what actually exercises the recursive mutex the
// registry has been carrying since W1, and the rules in registry.h stop being
// theoretical:
//   * registry calls from a handler go through the ordinary public API;
//   * a response document is built FIRST and sent afterwards, so no registry
//     lock is ever held across a network write;
//   * the session table and the event ring have their own locks, and the lock
//     ORDER is fixed (see the block comment in mod_http.cpp).
//
// ---- AUTH: TWO INDEPENDENT FACTORS --------------------------------------
//
// Stuart's decision, 2026-08-17. Joining the AP is not authentication:
//   1. WPA2-PSK with a random 100-bit passphrase generated at first enable and
//      persisted in NVS. NEVER derived from the MAC — the MAC is in every
//      beacon frame the device transmits.
//   2. A random 8-digit PIN, also persisted, exchanged at POST /api/session for
//      a 192-bit session token. Rate-limited with escalating delay and a
//      lockout; compared in constant time.
// Both secrets are readable ONLY at AUTH_PHYSICAL (i.e. over the USB cable),
// and neither ever appears in a log line, an error message, or any response
// below AUTH_TOKEN.
//
// `AUTH_PHYSICAL` is never granted to a network client, whatever it presents.
// That level means "is holding the cable", and a token cannot prove that.
//
// Claims RES_WIFI SHARED, so a future `wifiscan` in passive mode coexists and
// only monitor mode (which takes RES_WIFI EXCLUSIVE) evicts it — by
// arbitration, with neither module naming the other.
//
// defaultEnabled is FALSE and must stay false: a device that broadcasts an AP
// out of the box is a device that broadcasts an AP in someone's pocket.

#pragma once

#include "registry.h"

// Static descriptor; safe to hand straight to Registry::add().
const ModuleDescriptor *httpModuleDescriptor();

// Plain scheduler task — register it with Scheduler::addTask() from setup(),
// NOT as the module's `.tick`. The difference is the entire reason it exists:
// a module tick reaches the module through Registry::tickAt(), which holds the
// registry lock, and this work must run WITHOUT it.
//
// It completes a deferred shutdown. httpd_stop() joins the HTTP server task, so
// calling it while the registry lock is held would deadlock against a handler
// blocked on that same lock (see the shutdown block comment in mod_http.cpp).
// When disable() detects an in-flight request it defers the teardown here.
//
// Cheap: one atomic load per pass in the normal case, and it must keep being
// called even when the module is disabled — which is exactly when it has work.
void httpTransportPoll();
