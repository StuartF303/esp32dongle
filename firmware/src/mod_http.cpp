#include "mod_http.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <bootloader_random.h>
#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>

#include "apgrace.h"
#include "authfmt.h"
#include "bus.h"
#include "console.h"
#include "ct.h"
#include "modauth.h"
#include "otaupload.h"
#include "pairing.h"
#include "pinpolicy.h"
#include "protocol.h"
#include "ratelimit.h"
#include "registry.h"
#include "webui.h"

namespace {

// ===========================================================================
// THREADING — read this before touching anything below
// ===========================================================================
//
// TWO TASKS. The Arduino loop task (priority 1) runs setup(), the scheduler,
// every module tick and every CDC command. esp_http_server runs its OWN task
// (priority 5, one task for ALL sockets, created by httpd_start) and every URI
// handler, WebSocket frame and queued work item runs on it. Before this file
// existed the whole image was single-tasked by construction.
//
// THE HTTP SERVER TASK IS SINGLE. esp_http_server select()s over its sockets
// and services them one at a time on that one task. Everything that follows
// from that is load-bearing here:
//   * rxBuf_ is ONE buffer shared by POST /api/cmd and the /ws handler. Safe
//     only because two handlers can never run at once. If a second httpd
//     instance is ever started, or IDF gains a worker-per-socket mode, this
//     buffer must be split.
//   * WebSocket sends need no serialisation of their own: every send in this
//     file happens on that task, either from a handler or from drainEvents()
//     via httpd_queue_work().
//   * The ONE send we do not control is esp_http_server's automatic PONG reply
//     to a client PING, which it emits from the same task — so still no
//     interleaving. TEST FOR: a client that PINGs hard while events stream.
//
// LOCK ORDER, fixed, and the reason each rule exists:
//
//   registry lock  >  ringLock_        (a module emits an event from inside a
//                                       registry-locked dispatch)
//   registry lock  >  authLock_        (the module's own status()/dispatch()
//                                       run under the registry lock and read
//                                       the session table)
//   registry lock  >  wsLock_          (status() reports the client count)
//
// and NOTHING in this file ever takes the registry lock while holding
// authLock_, wsLock_ or ringLock_. The HTTP handlers therefore authenticate
// FIRST — taking and RELEASING authLock_ — and only then call into
// Console::execute()/registry. That asymmetry is what removes the cycle; it is
// not stylistic. A handler that held authLock_ across a dispatch would
// deadlock against a `sessions` command arriving over CDC at the same moment.
//
// NO REGISTRY LOCK IS HELD ACROSS A NETWORK WRITE. This file never takes the
// registry lock explicitly at all: Registry::list() and Registry::dispatch()
// take and release it internally, and every handler builds its whole response
// document BEFORE the first byte goes out. The only send that happens with any
// lock held is none — drainEvents() copies a frame out of the ring, releases
// ringLock_, and only then sends.
//
// THE BUS SINK RUNS ON THE LOOP TASK, usually with the registry lock held. It
// therefore must not block: it memcpy's into a fixed ring and pokes
// httpd_queue_work(), which is a non-blocking sendto() on the server's control
// socket (CONFIG_HTTPD_QUEUE_WORK_BLOCKING is not set in this SDK config, so it
// returns an error rather than waiting). It never touches a client socket.
//
// SHUTDOWN ORDERING IS LOAD-BEARING: httpd_stop() FIRST (it joins the server
// task, so no handler and no queued work item can be running afterwards), then
// the AP, then free the buffers. Freeing rxBuf_ or the ring while a handler
// still holds it is a use-after-free that would only ever show up under load.
// And httpd_stop() itself must not be called with the registry lock held —
// see the shutdown block further down, which is the one genuine deadlock in
// this design and the reason main.cpp registers a non-module scheduler task.

// ===========================================================================
// Bounds. Every one of them is enforced with a distinct code, and none of them
// truncates silently.
// ===========================================================================

// Largest POST /api/cmd body. Protocol::MAX_LINE, not a number of its own —
// protocol.h exists exactly so a request that works over CDC works here.
constexpr size_t MAX_BODY = Protocol::MAX_LINE;
// Largest inbound WebSocket frame. Same limit, same reason.
constexpr size_t MAX_WS_RX = Protocol::MAX_LINE;
// ONE buffer serves both (see the threading note), so the two limits must not
// drift apart: rxBuf_ is allocated at MAX_WS_RX + 1 and POST /api/cmd reads
// MAX_BODY + 1 into it.
static_assert(MAX_BODY == MAX_WS_RX, "rxBuf_ is shared by /api/cmd and /ws; their limits must be identical");
// Largest body accepted at POST /api/session. A PIN is 4 characters; anything
// approaching this is someone probing.
constexpr size_t MAX_SESSION_BODY = 256;
// Largest single WebSocket response we will build. `modules` with a full
// descriptor set is the big one (~4 KB today). Heap, transient, and bounded so
// a pathological response cannot eat the heap Wi-Fi needs.
constexpr size_t MAX_WS_TX = 16384;
// "Bearer " + 48 hex + NUL, with room to spare. A longer Authorization header
// is rejected by httpd itself (ESP_ERR_HTTPD_RESULT_TRUNC) and treated as no
// credential at all.
constexpr size_t MAX_AUTH_HDR = 96;

// ONE SESSION. Stuart's decision, 2026-08-24 (ARCHITECTURE.md, "Pairing
// model"), and it is the same decision as AP_MAX_CLIENTS == 1 below: with one
// AP slot there is exactly one client, so a second session slot could only ever
// hold a token that nothing on the air can present. Making that explicit is
// what lets the Wi-Fi association BE the session boundary (apgrace.h), which is
// in turn what makes a 4-digit PIN defensible.
//
// The table, the loops over it and the eviction path all still work for any
// value >= 1 and are written that way on purpose — but nothing may now ASSUME
// a second slot exists. The one place that shows is createSession(): a full
// table evicts, and with MAX_SESSIONS == 1 that means a successful PIN exchange
// TAKES OVER the existing session rather than sitting alongside it. That is the
// intended single-session behaviour (re-pairing displaces the old pairing) and
// it is reported to the caller as `evicted_oldest`.
constexpr uint8_t MAX_SESSIONS = 1;
constexpr uint8_t MAX_WS_CLIENTS = 3;
// Sockets esp_http_server may hold open at once. Below LWIP_MAX_SOCKETS (16)
// with room for the DHCP server and the control socket.
constexpr uint16_t MAX_OPEN_SOCKETS = 6;

// Session lifetimes. Two bounds, because either alone is wrong: idle-only lets
// a token live forever if the page is left open, absolute-only leaves a
// forgotten phone authenticated for hours.
constexpr uint32_t SESSION_IDLE_MS = 15u * 60u * 1000u;        // 15 minutes
constexpr uint32_t SESSION_ABSOLUTE_MS = 4u * 60u * 60u * 1000u;  // 4 hours

// Event ring. Allocated with the server, freed with it, so a disabled module
// costs nothing but its descriptor.
constexpr size_t EVENT_SLOT = 512;
constexpr uint8_t EVENT_SLOTS = 6;

// AP. Channel 6 is the middle of the three non-overlapping 2.4 GHz channels;
// nothing here scans first, so it is a fixed choice rather than a clever one.
constexpr int AP_CHANNEL = 6;
// SINGLE-CLIENT. Stuart's decision, 2026-08-24. Two things follow from it and
// both are load-bearing:
//
//   * It is half of what makes a 4-digit PIN defensible (authfmt.h). While the
//     owner's phone holds the one slot, nobody else can associate to guess at
//     all, so the brute-force window exists only while the device is unpaired.
//   * It makes "a station left" mean "the session is over" (apgrace.h). With
//     four slots that inference is not available, because the departing station
//     need not be the one holding the session.
//
// WHAT IT COSTS, stated rather than buried: anyone in radio range who knows the
// passphrase can squat the single slot and deny pairing, with no recovery
// except the USB cable. That is not a new exposure — under an owner-chosen,
// SSID-derived passphrase (see the psk_ note below) a squatter grinding the PIN
// was already occupying a slot — but it is now a denial of service that costs
// the attacker one association instead of four.
constexpr int AP_MAX_CLIENTS = 1;

// The module tick: reaps expired sessions and services a deferred reboot.
constexpr uint32_t TICK_MS = 250;
// How long after the response goes out a `reboot` command actually restarts.
constexpr uint32_t REBOOT_DELAY_MS = 400;

constexpr const char *NVS_NAMESPACE = "httpap";
constexpr const char *NVS_KEY_PSK = "psk";
// 0 / absent == device-generated, 1 == chosen by the owner via `psk set`.
// A separate key rather than inferring from the string's shape: an owner is
// free to type something that happens to look generated, and the read response
// must not lie about where the passphrase came from.
constexpr const char *NVS_KEY_PSK_SRC = "psksrc";
// THIS KEY IS NEVER WRITTEN. It survives only so enable() can ERASE it: builds
// before 2026-08-24 persisted the pairing PIN here, and an 8-digit PIN left
// sitting in flash from one of those is a stored credential this design no
// longer has any use for. Removing the constant would leave that old key
// unreachable and therefore permanent.
//
// The PIN is RAM-ONLY now (stuart, ARCHITECTURE.md "Pairing model"). A value
// that is minted fresh on every enable has no reason to survive a boot, and not
// writing it also removes a flash-wear vector — the same one ratelimit.h
// refuses to introduce by persisting failure counts.
constexpr const char *NVS_KEY_PIN = "pin";

// ===========================================================================
// State
// ===========================================================================

httpd_handle_t server_ = nullptr;
bool apUp_ = false;
bool sinkRegistered_ = false;

char ssid_[24] = {0};
// SECRETS. Both are AUTH_PHYSICAL-only and go to exactly two places: the `psk`
// / `pin` actions (which check the level first) and, for the PSK alone, NVS.
// They are never logged, never emitted as an event, and never rendered into
// status(). The one further route out of this file is the PIN's, and it is
// deliberately not a wire route: Pairing::publish() hands it to the LCD through
// an in-RAM rendezvous with no JSON representation (pairing.h).
//
// THE TWO NO LONGER HAVE THE SAME LIFETIME (2026-08-24). The PSK is persisted
// and is expected to outlive reboots; the PIN is RAM-only, minted on every
// enable, and rotated on session end and on every rate-limiter lockout. Code
// that treats them as one kind of thing — one NVS write block, one "refuse if
// it will not persist" guard — is now wrong, and loadOrCreateCredentials()
// below says so where it used to do exactly that.
//
// ---- THE OWNER-CHOSEN PASSPHRASE, AND WHAT IT COSTS --------------------
//
// `psk set` lets the device owner replace the generated passphrase with one
// they pick. Stuart asked for this after a 20-character generated PSK was
// mistyped into a phone and the WPA2 association failed, and he chose a value
// derived from the SSID. He was told, before choosing, exactly what follows,
// and reaffirmed it. This is a deliberate, informed decision by the owner of
// the device — not an oversight, and not something to "fix" in a later commit
// without asking him first.
//
// What it costs, plainly:
//   * A passphrase derivable from the SSID is a PUBLIC secret. The SSID goes
//     out in every beacon frame, so anyone in radio range who knows the scheme
//     knows the passphrase.
//   * WPA2-PSK gives NO forward secrecy against someone holding the
//     passphrase. A listener who captures the 4-way handshake — which is
//     replayable at will, by deauthing a client — derives the PTK and decrypts
//     everything on the air afterwards.
//   * Everything therefore means everything: the pairing PIN in the
//     POST /api/session response, the session token it returns, and every
//     command and event on the WebSocket. AUTH_PHYSICAL still holds the psk/pin
//     actions themselves, but that is no longer the only way to learn them.
//
// The mitigations that remain are the PIN rate limiter (ratelimit.h), the
// session lifetimes, and the fact that the AP is never default-enabled.
// The full-strength alternative is `psk` with no `set` — the generated
// passphrase, which is 64.4 bits and unrelated to the SSID.
//
// Sized for the WPA2 maximum, not for the generated form: an owner-chosen
// passphrase may be up to 63 characters.
char psk_[AuthFmt::PSK_MAX + 1] = {0};
// True when psk_ was chosen by the owner rather than minted here. Guarded by
// authLock_, mirrored in NVS under NVS_KEY_PSK_SRC.
bool pskSet_ = false;
char pin_[AuthFmt::PIN_LEN + 1] = {0};

// ---- shutdown coordination: the one genuine deadlock in this design -----
//
// THE HAZARD. Registry::disable() holds the registry lock across our
// httpDisable() (registry.h says so explicitly). httpd_stop() JOINS the HTTP
// server task. If that task is at that moment blocked inside
// Registry::dispatch() waiting for the very lock the loop task is holding, the
// two wait on each other forever and the task watchdog takes the device down.
// `disable http` typed over CDC while a phone is mid-request is all it takes.
//
// THE FIX, in two parts:
//
//   * A Dekker handshake decides whether a synchronous stop is SAFE. The
//     handler does `inRegistry_++` then reads `stopping_`; disable() does
//     `stopping_ = true` then reads `inRegistry_`. Both are seq_cst, so at
//     least one side sees the other: if disable() reads zero, no handler is in
//     — or can still enter — a registry call, and httpd_stop() cannot deadlock.
//     That is the overwhelmingly common case and it stays fully synchronous.
//
//   * Otherwise the teardown is DEFERRED to httpTransportPoll(), a plain
//     scheduler task registered by main.cpp. Plain scheduler tasks run straight
//     from loop() and do NOT hold the registry lock (unlike module ticks, which
//     the registry wraps in its Guard) — so httpd_stop() there joins a task
//     that can actually make progress.
//
// THE COST, stated plainly: in that deferred case `disable http` returns
// success while the AP and server are still up for up to one poll interval
// (20 ms). The module is marked disabled and RES_WIFI is released before the
// radio is actually down. A re-enable during that window is REFUSED with a
// clear message rather than racing the teardown. Closing the window properly
// would need an asynchronous disable in the registry contract — stuart's call,
// not this file's.
std::atomic<bool> stopping_{false};
std::atomic<int> inRegistry_{0};
std::atomic<bool> teardownPending_{false};

// Deferred restart, requested on the HTTP task and executed on the loop task.
//
// THE ORDER OF THESE TWO STORES IS LOAD-BEARING and the atomic is not
// decoration: httpTick() reads the flag and then the deadline, on the OTHER
// core. If the flag became visible first, the tick would see a stale
// rebootAtMs_ (0 at boot), find the deadline already past, and restart the chip
// while the response was still in the socket buffer — i.e. `reboot` over HTTP
// would look like a hang. Deadline first, release-store the flag, acquire-load
// it in the tick.
//
// CURRENTLY UNREACHABLE, deliberately kept. Backlog S1 put `reboot` at
// AUTH_PHYSICAL (cmdauth.h) and this transport dispatches at AUTH_TOKEN — a
// level it can never exceed, because nothing arriving over a radio can prove it
// is holding the cable. So Console::execute() refuses `reboot` here with EAUTH
// and never returns true. The machinery stays because it is the only correct
// way to restart from a request handler if that policy is ever revisited, and
// because deleting a working cross-core handshake to re-derive it later is a
// bad trade.
char rebootReason_[8] = {0};
std::atomic<bool> rebootPending_{false};
uint32_t rebootAtMs_ = 0;
// True when THIS enable() had to mint a PASSPHRASE, i.e. the operator has a new
// secret to go and read over USB. Reported by status(); the secrets themselves
// are not.
//
// It used to mean "a PSK or a PIN was minted". That stopped carrying any
// information the moment the PIN became RAM-only, because a fresh PIN is now
// minted on EVERY enable — the flag would simply always be true. The PIN needs
// no such flag anyway: it is on the LCD whenever the device is pairable
// (pairing.h), which is the whole point of having a screen.
bool credentialsNew_ = false;
// True when an old build's persisted `pin` key could not be erased from NVS
// (see loadOrCreateCredentials). Not fatal — the PIN in use is the fresh RAM
// one either way — but a stale credential left in flash must not be a SILENT
// condition, so status() reports it and there is something to go and look at.
bool stalePinKey_ = false;

char *rxBuf_ = nullptr;  // MAX_WS_RX + 1, httpd task only

struct Session {
  bool used;
  uint32_t id;
  char token[AuthFmt::TOKEN_LEN + 1];
  uint32_t createdMs;
  uint32_t lastSeenMs;
};
Session sessions_[MAX_SESSIONS] = {};
uint32_t sessionCounter_ = 0;
uint32_t sessionsIssued_ = 0;
uint32_t sessionsExpired_ = 0;
uint32_t sessionsEvicted_ = 0;
RateLimit::State pinLimit_;
// The 90-second window between the single AP client disassociating and the
// session ending (apgrace.h). Updated ONLY from httpTick(), i.e. the loop task,
// under authLock_ — which is also what makes it safe to read from status() on
// the HTTP task. Deliberately not a Wi-Fi event handler: that would mutate
// session state from the Wi-Fi task and add a third writer to this table for no
// gain, since the tick already runs every 250 ms.
ApGrace::State grace_;
SemaphoreHandle_t authLock_ = nullptr;  // sessions_ + pinLimit_ + psk_/pin_ + grace_

struct WsClient {
  bool used;
  int fd;
  bool authed;
  uint32_t sessionId;
  uint32_t openedMs;
};
WsClient wsClients_[MAX_WS_CLIENTS] = {};
uint32_t wsRejected_ = 0;
SemaphoreHandle_t wsLock_ = nullptr;  // wsClients_ only; never held across a send

struct EventSlot {
  uint16_t len;
  char data[EVENT_SLOT];
};
EventSlot *ring_ = nullptr;
uint8_t ringHead_ = 0, ringTail_ = 0, ringCount_ = 0;
uint32_t eventsDropped_ = 0;
uint32_t eventsSent_ = 0;
SemaphoreHandle_t ringLock_ = nullptr;

// RAII, for the same reason registry.cpp has one: these functions have many
// early returns and a hand-written take/give pair leaks the mutex the first
// time someone adds another. A null handle degrades to no locking rather than
// crashing.
class Lock {
 public:
  explicit Lock(SemaphoreHandle_t h) : h_(h) {
    if (h_ != nullptr) {
      xSemaphoreTake(h_, portMAX_DELAY);
    }
  }
  ~Lock() {
    if (h_ != nullptr) {
      xSemaphoreGive(h_);
    }
  }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;

 private:
  SemaphoreHandle_t h_;
};

// Marks the calling handler as being about to enter (or already inside) a
// registry call. Returns false if the module is shutting down, in which case
// the handler must answer 503 and touch nothing. See the shutdown note above —
// the ORDER of the increment and the load is the whole mechanism.
bool enterRegistry() {
  inRegistry_.fetch_add(1, std::memory_order_seq_cst);
  if (stopping_.load(std::memory_order_seq_cst)) {
    inRegistry_.fetch_sub(1, std::memory_order_seq_cst);
    return false;
  }
  return true;
}

void exitRegistry() { inRegistry_.fetch_sub(1, std::memory_order_seq_cst); }

// ===========================================================================
// Entropy
// ===========================================================================
//
// THIS IS THE WHOLE BALLGAME. esp_random() is only a true hardware RNG once
// the RF subsystem (Wi-Fi or BT) is running; with the radio off it degrades to
// a much weaker source, and a PSK or PIN drawn from that is guessable by anyone
// who knows the boot sequence.
//
// WHAT THIS FILE DOES, and why: credentials are generated in enable(), BEFORE
// WiFi.softAP() — they have to be, because the PSK is an input to the AP
// configuration — and the generation is bracketed by
// bootloader_random_enable()/bootloader_random_disable(). That is the
// documented way to get a real entropy source with the radio down: it clocks
// the SAR ADC into the RNG. The disable() call is not optional; leaving the
// bootloader entropy source running when Wi-Fi starts is explicitly
// unsupported, and ADC use afterwards would be affected too.
//
// SESSION TOKENS take the other route. They are only ever minted while the AP
// is up, i.e. with the radio running, so plain esp_random() is already a TRNG
// there and no bracketing is needed (and would be wrong — see above).
void rngBytes(uint8_t *out, size_t n) { esp_fill_random(out, n); }

// ===========================================================================
// Identity and credentials
// ===========================================================================

void buildSsid() {
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  // The MAC tail is already in every beacon frame this device transmits, so
  // putting it in the SSID leaks nothing new — it just makes two dongles on one
  // bench distinguishable. The PSK is emphatically NOT derived from it.
  snprintf(ssid_, sizeof(ssid_), "tdongle-%02x%02x", mac[4], mac[5]);
}

// Loads the PSK from NVS (generating and persisting it if it is missing or
// malformed), MINTS A FRESH PIN unconditionally, and erases any `pin` key an
// older build left in flash. `err` gets a short reason on failure.
//
// A stored PSK that fails its validator is REGENERATED rather than used: a
// half-written or truncated PSK would otherwise become an AP passphrase nobody
// can predict OR read, and the only recovery would be a reflash.
//
// The PSK's validator is now the WPA2 rule (8..63 printable ASCII), not the
// generated format — it has to be, because an owner-chosen passphrase is
// arbitrary. The corollary, stated rather than hidden: a truncation that still
// leaves 8+ printable characters is no longer detectable here and would be
// used as-is. `psk` at AUTH_PHYSICAL is what reveals that, and the owner can
// always re-set or clear it.
//
// ---- THE PIN IS NOT LOADED. IT IS MINTED. (2026-08-24) ------------------
//
// This function used to treat the two credentials identically: read both, mint
// whichever failed its validator, write both back in one block. It does not any
// more, and the asymmetry is the design rather than an oversight.
//
//   * MINT TRIGGER 1 OF 4 IS RIGHT HERE. Power-up and every `enable http` get a
//     brand new PIN, because nothing is read back. The other three are
//     consumption at pairing and the rate-limiter lockout, both in
//     handleSessionCreate(), and session end — the grace-window expiry in
//     httpTick(), or an explicit unpair via DELETE /api/session or `sessions
//     revoke`. Together they are what makes 10^4 defensible (authfmt.h). This
//     one is the cheapest of the four to get wrong silently — a single stray
//     getString() would quietly restore a permanent PIN — so it is stated
//     rather than left to be inferred from an absence.
//   * ANY OLD `pin` KEY IS REMOVED FROM THE NAMESPACE — AND THAT IS NOT THE
//     SAME AS ERASING THE BYTES. Measured on the real device (nvs partition
//     dumped either side of the call): Preferences::remove() changed exactly
//     one byte in 32 KB, 0x39 going 0xa8 -> 0x80, which is the page's two-bit
//     entry-state bitmap flipping those slots to ERASED. The data entry was
//     byte-identical afterwards and the old 8-digit PIN was still readable at
//     nvs offset 0x0d00. It stays there until NVS garbage-collects the page,
//     which happens when the page fills — on its own schedule, not ours.
//
//     WHAT THIS ACTUALLY BUYS, stated so nobody re-derives it from the name:
//     the key is gone from the API, so no code path — ours or a future one
//     that gets the idea to read it — can obtain it, and `nvs_get_str` returns
//     NOT_FOUND. What it does not buy is the disappearance of the digits from
//     the flash of a device someone can dump.
//
//     SCRUBBING IS NOT AVAILABLE AND MUST NOT BE ATTEMPTED. NVS is
//     log-structured: writing an overwrite value APPENDS a new entry and marks
//     the old one erased, so "overwrite it with zeros first" leaves the
//     original bytes exactly where they were and adds a copy of the zeros. The
//     only real scrub is erasing the whole partition, which takes the Wi-Fi
//     PSK and every module's enable state with it — a far worse outcome than a
//     stale 8-digit PIN that no longer opens anything.
//
//     THE PROPERTY THAT ACTUALLY MATTERS is one line further down: no NEW PIN
//     is ever written to flash at all. The residue is bounded, one-off, and
//     belongs to a build that no longer exists; the design change is that the
//     credential stopped being persisted. Logged in docs/BACKLOG.md as an
//     accepted residue so the next person to dump the partition finds an
//     explanation rather than a surprise.
//
//     The read-only pass checks isKey() first, so the common case costs no NVS
//     write and no flash wear.
//   * THE "REFUSE RATHER THAN RUN WITH A CREDENTIAL THAT WILL NOT PERSIST"
//     GUARD NOW APPLIES TO THE PASSPHRASE ONLY. It was written for both, and
//     for the PSK the reasoning still holds exactly: "the passphrase I wrote
//     down stopped working after a reboot" is a far worse failure than "it
//     would not start and said why". For the PIN that reasoning is now
//     inverted — a PIN that does not persist is CORRECT, so an NVS failure
//     around it must not block the AP from starting. Nobody writes this PIN
//     down; it is on the LCD, and the next enable mints another one.
bool loadOrCreateCredentials(char *err, size_t errCap) {
  credentialsNew_ = false;
  stalePinKey_ = false;
  psk_[0] = '\0';
  pin_[0] = '\0';
  pskSet_ = false;

  bool stalePin = false;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    prefs.getString(NVS_KEY_PSK, psk_, sizeof(psk_));
    pskSet_ = prefs.getUChar(NVS_KEY_PSK_SRC, 0) != 0;
    // NOT getString(): the value is of no interest and copying an old PIN into
    // RAM to decide whether to delete it would defeat the point of deleting it.
    stalePin = prefs.isKey(NVS_KEY_PIN);
    prefs.end();
  }
  psk_[sizeof(psk_) - 1] = '\0';

  // sizeof(psk_), not the default scan cap: psk_ is PSK_MAX + 1 bytes and
  // walking further would read past it (-Wstringop-overread). 64 is still
  // enough to tell a legal 63 from an over-length value.
  bool needPsk = !AuthFmt::validPassphrase(psk_, sizeof(psk_));
  if (needPsk) {
    pskSet_ = false;  // whatever the flag said, what we are about to use is ours
  }

  // Radio is DOWN at this point (enable() calls this before WiFi.softAP), which
  // is exactly the condition under which esp_random() needs help. See the
  // entropy note above. The PIN is drawn here for the same reason the PSK is:
  // this is the only moment in the module's life when the bracketing is both
  // necessary and legal.
  bootloader_random_enable();
  bool okPsk = !needPsk || AuthFmt::makePsk(psk_, sizeof(psk_), rngBytes);
  bool okPin = AuthFmt::makePin(pin_, sizeof(pin_), rngBytes);
  bootloader_random_disable();

  if (!okPsk || !okPin) {
    snprintf(err, errCap, "the RNG would not produce a usable %s; refusing to start with a weak credential",
             !okPsk ? "passphrase" : "PIN");
    psk_[0] = '\0';
    pin_[0] = '\0';
    return false;
  }

  if (!needPsk && !stalePin) {
    // Nothing to write and nothing to erase — the overwhelmingly common path,
    // and it touches NVS read-only. The PIN minted above simply lives in RAM.
    return true;
  }

  Preferences w;
  if (!w.begin(NVS_NAMESPACE, false)) {
    if (needPsk) {
      snprintf(err, errCap, "cannot open NVS namespace '%s' to store the AP passphrase", NVS_NAMESPACE);
      return false;
    }
    // Only the stale-PIN erase wanted the write handle. Refusing to start over
    // that would take the AP down for a key this build never reads.
    stalePinKey_ = true;
    return true;
  }
  bool wrote = true;
  if (needPsk) {
    wrote = wrote && w.putString(NVS_KEY_PSK, psk_) == strlen(psk_);
    // Freshly minted, so the source flag must say so — otherwise a device that
    // once had an owner-chosen passphrase and then lost it (corrupt NVS entry,
    // factory reset of that key alone) would keep claiming "set".
    wrote = wrote && w.putUChar(NVS_KEY_PSK_SRC, 0) == sizeof(uint8_t);
  }
  // No putString(NVS_KEY_PIN, ...) here, and there must never be one again.
  bool erased = !stalePin || w.remove(NVS_KEY_PIN);
  w.end();
  if (!wrote) {
    // The PASSPHRASE, and only the passphrase. See the guard note above.
    snprintf(err, errCap,
             "NVS write of the AP passphrase failed; not starting with a passphrase that would not persist");
    return false;
  }
  stalePinKey_ = !erased;
  credentialsNew_ = needPsk;
  return true;
}

// Mints a fresh PIN into pin_. CALLER HOLDS authLock_.
//
// Deliberately does NOTHING else — no session revoke, no rate-limiter reset, no
// NVS. The three callers want different combinations of those and conflating
// them is precisely how the lockout path would end up clearing the limiter it
// was triggered by. Each caller states what it wants at the call site.
//
// Into a temporary first, then copied: AuthFmt::makePin() writes digits into
// its output buffer as it draws them and only terminates on success, so
// generating straight into pin_ would leave a corrupt PIN behind on RNG
// failure. "The old PIN is unchanged" is a recoverable outcome; "the PIN is now
// three digits and nothing on the LCD works" is not.
bool mintPinLocked(char *err, size_t errCap) {
  char fresh[AuthFmt::PIN_LEN + 1];
  // The radio is UP here (the module is enabled), so esp_random() is already a
  // TRNG and bootloader_random_enable() must NOT be called — doing so with
  // Wi-Fi running is unsupported and would disturb the ADC.
  if (!AuthFmt::makePin(fresh, sizeof(fresh), rngBytes)) {
    snprintf(err, errCap, "the RNG would not produce a usable PIN; the old one is unchanged");
    return false;
  }
  memcpy(pin_, fresh, sizeof(fresh));
  memset(fresh, 0, sizeof(fresh));
  return true;
}

// Caller holds authLock_.
uint8_t revokeAllLocked() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
    if (sessions_[i].used) {
      sessions_[i] = Session{};
      n++;
    }
  }
  return n;
}

// The `pin regenerate:true` action: a new PIN, every session revoked, and a
// clean limiter. Caller must NOT hold authLock_.
//
// THE LIMITER RESET HERE IS DELIBERATE AND IS NOT SHARED WITH THE LOCKOUT PATH.
// Both decisions live in pinpolicy.h precisely so the two can be read side by
// side and neither can be "tidied" into the other: this action is AUTH_PHYSICAL
// (modauth.h), so the caller is holding the USB cable and is the device's owner
// rather than the party the limiter defends against, and clearing a 15-minute
// lockout for them is the documented way out of one. The lockout path in
// handleSessionCreate() must NOT clear — test_pinpolicy asserts both.
bool regeneratePin(char *err, size_t errCap) {
  Lock l(authLock_);
  if (!mintPinLocked(err, errCap)) {
    // Nothing has changed yet, so nothing to undo: the limiter is only cleared
    // below, after the new PIN is actually in place. An owner whose RNG just
    // failed keeps their old PIN AND their old limiter state, which is the
    // honest outcome — the alternative would clear a lockout in exchange for
    // nothing.
    return false;
  }
  revokeAllLocked();
  PinPolicy::operatorRegenerate(pinLimit_);
  return true;
}

// ===========================================================================
// Sessions
// ===========================================================================

bool sessionExpired(const Session &s, uint32_t now) {
  return (int32_t)(now - s.lastSeenMs) >= (int32_t)SESSION_IDLE_MS ||
         (int32_t)(now - s.createdMs) >= (int32_t)SESSION_ABSOLUTE_MS;
}

// Caller holds authLock_.
uint8_t reapLocked(uint32_t now) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
    if (sessions_[i].used && sessionExpired(sessions_[i], now)) {
      sessions_[i] = Session{};
      sessionsExpired_++;
      n++;
    }
  }
  return n;
}

// Validates a token and refreshes its idle timer. Returns the session id, or 0.
//
// Constant time in TWO senses: the comparison itself (CT::equalStr), and the
// walk — every slot is examined whatever matches, so the time taken does not
// reveal how many sessions exist or which slot holds the caller's.
uint32_t touchSession(const char *token, uint32_t now) {
  if (token == nullptr || !AuthFmt::validToken(token)) {
    return 0;
  }
  Lock l(authLock_);
  reapLocked(now);
  uint32_t found = 0;
  int8_t foundIdx = -1;
  for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions_[i].used) {
      continue;
    }
    if (CT::equalStr(sessions_[i].token, token, AuthFmt::TOKEN_LEN)) {
      found = sessions_[i].id;
      foundIdx = (int8_t)i;
    }
  }
  if (foundIdx >= 0) {
    sessions_[foundIdx].lastSeenMs = now;
  }
  return found;
}

// Is `id` a live session, and if so refresh its idle timer. Caller holds
// authLock_ and must have reaped first.
//
// NOT constant time, and it does not need to be: touchSession() above compares
// a TOKEN, which is attacker-supplied and secret, so its walk and its compare
// are both CT. This one takes a session id that THIS DEVICE assigned and that
// the caller read out of its own WebSocket table — an attacker cannot vary it
// per call, cannot observe the comparison without already holding the socket,
// and learns nothing from a timing difference that finding the socket in the
// table has not already told them.
int8_t sessionIndexLocked(uint32_t id) {
  if (id == 0) {
    return -1;
  }
  for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
    if (sessions_[i].used && sessions_[i].id == id) {
      return (int8_t)i;
    }
  }
  return -1;
}

bool touchSessionByIdLocked(uint32_t id, uint32_t now) {
  int8_t i = sessionIndexLocked(id);
  if (i < 0) {
    return false;
  }
  sessions_[i].lastSeenMs = now;
  return true;
}

// The same lookup WITHOUT the refresh, for the event fan-out. The distinction
// is deliberate: a client RECEIVING a push has not done anything, and counting
// it as activity would keep a session alive indefinitely for a page left open
// on a desk — the exact failure SESSION_IDLE_MS exists to prevent, and the one
// the file's session-lifetime note calls out. Only an inbound frame refreshes.
bool sessionLiveLocked(uint32_t id) { return sessionIndexLocked(id) >= 0; }

// Mints a session. Returns 0 on failure (which can only be an RNG failure —
// a full table evicts instead, see below).
uint32_t createSession(char *tokenOut, size_t cap, uint32_t now, bool *evicted) {
  *evicted = false;
  Lock l(authLock_);
  reapLocked(now);

  int8_t slot = -1;
  for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions_[i].used) {
      slot = (int8_t)i;
      break;
    }
  }
  if (slot < 0) {
    // Table full of LIVE sessions. Evict the least recently used rather than
    // refusing: refusing means a stale-but-unexpired session locks the rightful
    // owner out of their own device for up to SESSION_IDLE_MS, with the only
    // remedy being the USB cable. The eviction is REPORTED, not silent.
    //
    // WITH MAX_SESSIONS == 1 THIS IS THE RE-PAIRING PATH, not a rare overflow:
    // whoever presents the correct PIN takes the single session over from
    // whoever held it. That is the intended behaviour (the device has one
    // owner, and a phone that lost its token must be able to come back without
    // waiting out SESSION_IDLE_MS), and it is bounded by the PIN — which is on
    // the LCD only while the device is unpaired. The `oldest` loop below is
    // kept general rather than collapsed to `slot = 0`: it is correct for any
    // MAX_SESSIONS and costs one comparison at 1.
    uint32_t oldest = 0;
    slot = 0;
    for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
      uint32_t idle = now - sessions_[i].lastSeenMs;
      if (idle >= oldest) {
        oldest = idle;
        slot = (int8_t)i;
      }
    }
    *evicted = true;
    sessionsEvicted_++;
  }

  // Radio is up (the module is enabled), so esp_random() is a TRNG here.
  if (!AuthFmt::makeToken(tokenOut, cap, rngBytes)) {
    return 0;
  }
  Session &s = sessions_[slot];
  s.used = true;
  s.id = ++sessionCounter_;
  memcpy(s.token, tokenOut, AuthFmt::TOKEN_LEN + 1);
  s.createdMs = now;
  s.lastSeenMs = now;
  sessionsIssued_++;
  // A NEW SESSION GETS A NEW CLOCK, stated rather than left emergent. The
  // pairing client is associated at this instant, so httpTick()'s next
  // ApGrace::update() would clear the latch anyway via `stations > 0` — but
  // "would anyway" is exactly the kind of reasoning that stops being true when
  // someone changes the arming condition. Resetting here means the new session
  // cannot inherit anything from the one it replaced: not an armed deadline,
  // and not a `fired` latch that would suppress its first legitimate expiry.
  // Safe here because this function holds authLock_ for its whole body (taken
  // at the top), and grace_ is guarded by that same lock.
  ApGrace::reset(grace_);
  return s.id;
}

bool revokeSession(uint32_t id) {
  Lock l(authLock_);
  for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
    if (sessions_[i].used && sessions_[i].id == id) {
      sessions_[i] = Session{};
      return true;
    }
  }
  return false;
}

uint8_t revokeAllSessions() {
  Lock l(authLock_);
  return revokeAllLocked();
}

uint8_t liveSessionCount() {
  Lock l(authLock_);
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
    if (sessions_[i].used) {
      n++;
    }
  }
  return n;
}

// ===========================================================================
// WebSocket client table
// ===========================================================================

int8_t wsIndexOf(int fd) {
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
    if (wsClients_[i].used && wsClients_[i].fd == fd) {
      return (int8_t)i;
    }
  }
  return -1;
}

bool wsAdd(int fd, bool authed, uint32_t sessionId, uint32_t now) {
  Lock l(wsLock_);
  if (wsIndexOf(fd) >= 0) {
    return true;
  }
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
    if (!wsClients_[i].used) {
      wsClients_[i].used = true;
      wsClients_[i].fd = fd;
      wsClients_[i].authed = authed;
      wsClients_[i].sessionId = sessionId;
      wsClients_[i].openedMs = now;
      return true;
    }
  }
  wsRejected_++;
  return false;
}

void wsRemove(int fd) {
  Lock l(wsLock_);
  int8_t i = wsIndexOf(fd);
  if (i >= 0) {
    wsClients_[i] = WsClient{};
  }
}

// The session id this socket authenticated as, or 0 if it never did.
//
// wsIsAuthed() USED TO LIVE HERE and answered a weaker question — "did this
// socket ever present a valid token?" — which is not the question a command
// frame needs answered. See wsSessionStillLive().
uint32_t wsSessionOf(int fd) {
  Lock l(wsLock_);
  int8_t i = wsIndexOf(fd);
  return (i >= 0 && wsClients_[i].authed) ? wsClients_[i].sessionId : 0;
}

// ===========================================================================
// THE WEBSOCKET AUTHORISATION GATE
// ===========================================================================
//
// THE BUG THIS EXISTS TO FIX. Every command frame used to be gated on
// wsIsAuthed(fd) — a bool set once, when the socket sent its `auth` frame, and
// never revisited. wsClients_[i].sessionId was written and never read by
// anything. touchSession() therefore ran EXACTLY ONCE per socket, at the `auth`
// frame, and two things followed, both wrong:
//
//   * A SOCKET OUTLIVED THE SESSION THAT AUTHORISED IT. An unpair revoked the
//     token, rotated the PIN and put fresh digits on the LCD, while the socket
//     opened under the revoked session kept executing at AUTH_TOKEN — HID
//     injection, OTA image selection, everything Console::execute() offers.
//     Someone then reads the new PIN off the panel and pairs, and the device
//     has TWO live token-level capabilities against it. That directly
//     contradicts MAX_SESSIONS == 1, which is cited (with AP_MAX_CLIENTS == 1)
//     as half of what makes a 4-digit PIN defensible — the guarantee is not
//     "one session record exists" but "one party can act", and the second is
//     what the constant is worth.
//   * A WS-ONLY CLIENT NEVER REFRESHED ITS IDLE TIMER. Nothing called
//     touchSession() after the handshake, so a page that pairs and then drives
//     everything over /ws was reaped at SESSION_IDLE_MS while actively in use.
//     `live` went to 0, the LCD started advertising a PIN, and a second party
//     could pair — while the first socket carried on working, because of the
//     bug above. The two failures were the same missing lookup, and they cover
//     for each other, which is why neither is visible from the outside.
//
// FIXED IN THE GATE, NOT AT THE REVOKE SITES. Every path that ends a session
// would otherwise have to remember to hunt down sockets — four such paths
// today, and setPassphrase() already half-knew, being the only one that calls
// dropAllWsClients(). Re-checking here means a session ending is enough on its
// own, whatever ended it and whatever gets added later.
//
// LOCKING, and why it is two acquisitions rather than one nested pair: wsLock_
// and authLock_ have no order defined between them anywhere in this file, and
// this is not the place to invent one. The session id is copied out under
// wsLock_, that lock is RELEASED, and only then is authLock_ taken. Neither is
// held across wsSendText() or Bus::emit().
//
// COST PER FRAME: two uncontended FreeRTOS mutex take/give pairs, a scan of
// MAX_WS_CLIENTS (3) and a scan of MAX_SESSIONS (1), plus the reap that
// touchSession() would have done anyway. Tens of microseconds on a 240 MHz
// core, against a frame that is about to be JSON-parsed and dispatched through
// the registry — i.e. lost in the noise of the work it authorises. It is not
// worth caching, and a cache would reintroduce exactly the staleness this
// removes.
bool wsSessionStillLive(int fd, uint32_t now) {
  uint32_t id = wsSessionOf(fd);
  if (id == 0) {
    return false;
  }
  Lock l(authLock_);
  reapLocked(now);
  return touchSessionByIdLocked(id, now);
}

void wsSetAuthed(int fd, uint32_t sessionId) {
  Lock l(wsLock_);
  int8_t i = wsIndexOf(fd);
  if (i >= 0) {
    wsClients_[i].authed = true;
    wsClients_[i].sessionId = sessionId;
  }
}

uint8_t wsCount() {
  Lock l(wsLock_);
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
    if (wsClients_[i].used) {
      n++;
    }
  }
  return n;
}

// httpd task only. Never called with any of this file's locks held.
esp_err_t wsSendText(int fd, const char *s, size_t len) {
  if (server_ == nullptr) {
    return ESP_FAIL;
  }
  httpd_ws_frame_t f;
  memset(&f, 0, sizeof(f));
  f.final = true;
  f.fragmented = false;
  f.type = HTTPD_WS_TYPE_TEXT;
  f.payload = (uint8_t *)s;
  f.len = len;
  return httpd_ws_send_frame_async(server_, fd, &f);
}

// Serialises `doc` to the heap and sends it as one text frame. Heap rather than
// a static buffer because the big response (`modules`) is ~4 KB and only exists
// for microseconds — paying that in static RAM permanently, on a board with no
// PSRAM, to save one malloc is the wrong trade.
bool wsSendDoc(int fd, JsonDocument &doc) {
  size_t n = measureJson(doc);
  if (n + 1 > MAX_WS_TX) {
    // Never truncate: a half a JSON object is indistinguishable from a
    // transport bug at the far end.
    JsonDocument e;
    e["ok"] = false;
    JsonObject eo = e["e"].to<JsonObject>();
    eo["code"] = "ETOOBIG";
    eo["msg"] = "response does not fit a WebSocket frame; request it over REST instead";
    e["d"]["bytes"] = (uint32_t)n;
    e["d"]["max"] = (uint32_t)MAX_WS_TX;
    // 256 is measured against the fixed text above (~150 bytes) with room for
    // two decimal numbers. serializeJson() into a short buffer would truncate
    // silently, which is the exact failure this branch exists to prevent.
    char small[256];
    size_t m = serializeJson(e, small, sizeof(small));
    wsSendText(fd, small, m);
    return false;
  }
  char *buf = (char *)malloc(n + 1);
  if (buf == nullptr) {
    return false;
  }
  size_t m = serializeJson(doc, buf, n + 1);
  esp_err_t err = wsSendText(fd, buf, m);
  free(buf);
  return err == ESP_OK;
}

// ===========================================================================
// Event fan-out: loop task -> ring -> httpd task -> WebSocket clients
// ===========================================================================

// Runs on the HTTP server task, queued by the sink below.
void drainEvents(void *arg) {
  (void)arg;
  char frame[EVENT_SLOT];
  for (;;) {
    size_t len = 0;
    {
      Lock l(ringLock_);
      if (ring_ == nullptr || ringCount_ == 0) {
        return;
      }
      len = ring_[ringTail_].len;
      memcpy(frame, ring_[ringTail_].data, len);
      ringTail_ = (uint8_t)((ringTail_ + 1) % EVENT_SLOTS);
      ringCount_--;
    }
    // Lock released BEFORE the send: a slow or dead client must not stall the
    // loop task's next emit().
    int fds[MAX_WS_CLIENTS];
    uint32_t ids[MAX_WS_CLIENTS];
    uint8_t n = 0;
    {
      Lock l(wsLock_);
      for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
        if (wsClients_[i].used && wsClients_[i].authed) {
          fds[n] = wsClients_[i].fd;
          ids[n] = wsClients_[i].sessionId;
          n++;
        }
      }
    }

    // THE SAME QUESTION THE COMMAND GATE ASKS, because this is the same
    // capability from the other direction. Fixing only the gate would stop a
    // revoked socket EXECUTING while leaving it SUBSCRIBED: every module event,
    // every activity record, every OTA progress line and the `session_ended`
    // event announcing its own revocation would keep streaming to a party the
    // device believes it has unpaired. A socket whose session has gone is
    // dropped and closed here rather than waiting for it to send a frame,
    // because an idle listener never sends one.
    //
    // Two acquisitions again, never nested: the ids were copied out under
    // wsLock_, which is released above, and authLock_ is released before the
    // first send. No lock is held across wsSendText().
    bool live[MAX_WS_CLIENTS];
    {
      Lock l(authLock_);
      reapLocked(millis());
      for (uint8_t i = 0; i < n; i++) {
        live[i] = sessionLiveLocked(ids[i]);
      }
    }

    for (uint8_t i = 0; i < n; i++) {
      if (!live[i]) {
        wsRemove(fds[i]);
        httpd_sess_trigger_close(server_, fds[i]);
        continue;
      }
      if (wsSendText(fds[i], frame, len) == ESP_OK) {
        eventsSent_++;
      } else {
        // The socket is gone or wedged. Drop it from the table now; httpd's
        // close_fn will also fire, and both paths are idempotent.
        wsRemove(fds[i]);
        httpd_sess_trigger_close(server_, fds[i]);
      }
    }
  }
}

// Bus sink. Runs on WHICHEVER task emitted — usually the loop task, with the
// registry lock held — so it does the minimum: serialise into the ring and
// poke the server task. It must tolerate being called while this module is
// disabled, because Bus has no removeSink() (see bus.h): ring_ == nullptr is
// the "we are down" test and the event is dropped silently, which is the
// documented behaviour of a bus with no listener.
void eventSink(const char *name, Bus::FillFn fill, void *ctx) {
  if (ring_ == nullptr || server_ == nullptr) {
    return;
  }
  JsonDocument doc;
  doc["ev"] = name;
  JsonObject d = doc["d"].to<JsonObject>();
  if (fill != nullptr) {
    fill(d, ctx);
  }
  size_t n = measureJson(doc);

  {
    Lock l(ringLock_);
    if (ring_ == nullptr) {
      return;  // disabled between the check above and the lock
    }
    if (n + 1 > EVENT_SLOT || ringCount_ >= EVENT_SLOTS) {
      // Dropped, and COUNTED — reported by the `status` action. A silently
      // dropped event is a live data feed that quietly stops being live.
      eventsDropped_++;
      return;
    }
    EventSlot &slot = ring_[ringHead_];
    slot.len = (uint16_t)serializeJson(doc, slot.data, EVENT_SLOT);
    ringHead_ = (uint8_t)((ringHead_ + 1) % EVENT_SLOTS);
    ringCount_++;
  }

  // Non-blocking. If the control socket is momentarily full this returns an
  // error and the item simply waits in the ring for the next emit to queue a
  // drain — events are best-effort by design, and the heartbeat guarantees
  // another attempt within 5 s.
  httpd_queue_work(server_, drainEvents, nullptr);
}

// ===========================================================================
// HTTP plumbing
// ===========================================================================

// ArduinoJson writer that streams straight into HTTP chunked encoding. This is
// why `GET /api/modules` needs no response buffer at all: the document is
// serialised through a 256-byte window instead of into a 4 KB one.
class ChunkWriter {
 public:
  explicit ChunkWriter(httpd_req_t *req) : req_(req) {}

  size_t write(uint8_t c) {
    if (err_) {
      return 0;
    }
    buf_[n_++] = (char)c;
    if (n_ == sizeof(buf_)) {
      flush();
    }
    return err_ ? 0 : 1;
  }

  size_t write(const uint8_t *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
      if (write(s[i]) == 0) {
        return i;
      }
    }
    return len;
  }

  bool finish() {
    flush();
    return !err_;
  }

 private:
  void flush() {
    if (n_ > 0 && !err_) {
      if (httpd_resp_send_chunk(req_, buf_, n_) != ESP_OK) {
        err_ = true;
      }
    }
    n_ = 0;
  }

  httpd_req_t *req_;
  char buf_[256];
  size_t n_ = 0;
  bool err_ = false;
};

void setCommonHeaders(httpd_req_t *req) {
  // no-store on EVERYTHING: /api/session's response carries the session token,
  // and a cached copy of it in a phone browser outlives the session.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

esp_err_t sendJsonDoc(httpd_req_t *req, const char *status, JsonDocument &doc) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  setCommonHeaders(req);
  ChunkWriter w(req);
  serializeJson(doc, w);
  if (!w.finish()) {
    return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// The protocol's error object, over HTTP, with a matching status code. Same
// shape as every other error in this codebase — {"ok":false,"e":{code,msg}} —
// so a caller parses one thing whatever went wrong.
esp_err_t sendErr(httpd_req_t *req, const char *status, const char *code, const char *msg) {
  JsonDocument doc;
  doc["ok"] = false;
  JsonObject e = doc["e"].to<JsonObject>();
  e["code"] = code;
  e["msg"] = msg;
  return sendJsonDoc(req, status, doc);
}

enum BodyResult : uint8_t { BODY_OK, BODY_TOO_BIG, BODY_EMPTY, BODY_IO };

BodyResult readBody(httpd_req_t *req, char *buf, size_t cap, size_t *outLen) {
  *outLen = 0;
  size_t total = req->content_len;
  if (total == 0) {
    return BODY_EMPTY;
  }
  if (total > cap - 1) {
    return BODY_TOO_BIG;
  }
  size_t got = 0;
  uint8_t timeouts = 0;
  while (got < total) {
    int r = httpd_req_recv(req, buf + got, total - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts > 3) {
        return BODY_IO;
      }
      continue;
    }
    if (r <= 0) {
      return BODY_IO;
    }
    got += (size_t)r;
  }
  buf[got] = '\0';
  *outLen = got;
  return BODY_OK;
}

esp_err_t sendBodyError(httpd_req_t *req, BodyResult r, size_t cap) {
  char msg[128];
  switch (r) {
    case BODY_TOO_BIG:
      snprintf(msg, sizeof(msg), "request body is %u bytes; the limit is %u", (unsigned)req->content_len,
               (unsigned)(cap - 1));
      return sendErr(req, "413 Payload Too Large", "ETOOBIG", msg);
    case BODY_EMPTY:
      return sendErr(req, "400 Bad Request", "EARGS", "empty request body; a JSON object is required");
    default:
      return sendErr(req, "400 Bad Request", "EIO", "the request body could not be read from the socket");
  }
}

// Returns the caller's session id, or 0 for "no valid credential". NEVER holds
// authLock_ on return — see the lock-order note at the top of this file.
uint32_t authenticate(httpd_req_t *req) {
  char hdr[MAX_AUTH_HDR];
  // A header longer than this returns ESP_ERR_HTTPD_RESULT_TRUNC and is treated
  // as absent. A valid credential cannot be that long, so nothing legitimate is
  // lost and an unbounded header cannot be smuggled past the parser.
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
    return 0;
  }
  char tok[AuthFmt::TOKEN_LEN + 1];
  if (!AuthFmt::bearerToken(hdr, tok, sizeof(tok))) {
    return 0;
  }
  uint32_t id = touchSession(tok, millis());
  memset(tok, 0, sizeof(tok));
  return id;
}

esp_err_t send401(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"tdongle\"");
  return sendErr(req, "401 Unauthorized", "EAUTH",
                 "a session token is required: POST /api/session with the device PIN, then send "
                 "Authorization: Bearer <token>");
}

// ===========================================================================
// Auth events. No secret ever reaches these — they carry outcomes and counts.
// ===========================================================================

struct AuthEvt {
  const char *result;
  uint8_t remaining;
  uint32_t retryMs;
  uint32_t session;
  // True for the outcomes of a PIN ATTEMPT ("fail", "rate_limited", "locked"),
  // false for lifecycle outcomes ("ok", "session_ended"). It used to be
  // inferred from `session != 0`, which worked only while every non-attempt
  // event carried a session id — and "session_ended" may not: the grace window
  // can close with nothing in the table, and reporting
  // `attempts_remaining: 0` there would say the exact opposite of the truth.
  bool reportAttempts;
  // A fresh PIN was minted as part of THIS outcome. Not the PIN, obviously, and
  // not its length either — just the fact that whatever a client or an operator
  // last read is now void.
  bool pinRotated;
};

void fillAuthEvent(JsonObject d, void *ctx) {
  const AuthEvt *e = (const AuthEvt *)ctx;
  d["result"] = e->result;
  if (e->session != 0) {
    d["session"] = e->session;
  }
  if (e->reportAttempts) {
    d["attempts_remaining"] = e->remaining;
    if (e->retryMs != 0) {
      d["retry_after_ms"] = e->retryMs;
    }
  }
  if (e->pinRotated) {
    d["pin_rotated"] = true;
  }
}

// ---- THE RULE FOR ROTATING THE PIN AFTER A REVOCATION -------------------
//
// AN ACTUAL REVOCATION ROTATES THE PIN; ANYTHING THAT REVOKES NOTHING ROTATES
// NOTHING. That is the whole rule, it is stated once here rather than three
// times at three call sites, and every path that can end a pairing obeys it:
// the grace-window expiry in httpTick(), DELETE /api/session, `sessions
// revoke`, and the session cull inside `psk set`.
//
// WHY A REVOCATION MUST ROTATE. The PIN is single-use and pairing spent it
// (mint trigger 2). The instant the session ends, Pairing::shouldShow() becomes
// true again and httpTick() publishes pin_ to the LCD — so without a mint the
// panel would display a PIN that has already been used and cannot open
// anything. It would look exactly like a valid pairing code and fail silently,
// which is the worst shape a failure can take on a device whose only output is
// four digits on a screen.
//
// WHY "REVOKED NOTHING" MUST NOT. Rotating on a request that ended no pairing
// hands anyone who can reach the endpoint a way to churn the PIN — and the LCD
// — with calls that fail. `revoke:7` naming a dead id, or a DELETE whose
// session was reaped a moment earlier, changed nothing about the device's
// pairing state and must change nothing about its PIN.
//
// WHAT IS DELIBERATELY NOT AN INPUT: whether whatever came after the revocation
// then SUCCEEDED. `psk set` is the case that makes this explicit — it revokes
// every session, then restarts the AP, and that restart can fail and be
// reverted, on the air and in NVS. None of those branches matter here. The
// sessions are gone either way, so a PIN has been spent either way, and the
// question this function answers is only ever "was a pairing ended?". Making
// the rotation conditional on the restart's outcome would leave the failure
// branch showing a spent PIN.
//
// THE GUARD IS IN THE SIGNATURE, not at the call sites. `revoked` is how many
// sessions the caller actually revoked; 0 returns false and does nothing at
// all. It was a caller-side `n > 0 &&` conjunction at four sites, which is four
// chances to forget and a fifth site guaranteed to — and the failure mode is
// quiet: a PIN that churns, and an LCD that changes, on requests that ended
// nothing. Passing the count makes the rule impossible to express wrongly.
//
// Caller must NOT hold authLock_. `endedId` is the session that ended, or 0
// when there is no single meaningful id (a cull of several, or a re-key).
bool rotateAfterRevocation(uint8_t revoked, uint32_t endedId) {
  if (revoked == 0) {
    return false;
  }
  bool rotated;
  {
    Lock l(authLock_);
    char rerr[128];
    // A failed mint leaves the OLD (spent) PIN in place. Reported rather than
    // swallowed; the next enable, lockout or session end mints again.
    rotated = mintPinLocked(rerr, sizeof(rerr));
  }
  // Visible on the console and to every WebSocket client, in the same shape as
  // every other auth outcome. Never the PIN, never the token that just stopped
  // working.
  AuthEvt ev{"session_ended", 0, 0, endedId, false, rotated};
  Bus::emit("http.auth", fillAuthEvent, &ev);
  return rotated;
}

// ===========================================================================
// Handlers
// ===========================================================================

esp_err_t handlePage(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  setCommonHeaders(req);
  // Confines the page to this device: no third-party script, no off-device
  // fetch, and the WebSocket may only go back to the same origin.
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'");
  return httpd_resp_send(req, WEBUI_HTML, HTTPD_RESP_USE_STRLEN);
}

// The ONLY authenticated-free JSON endpoint besides POST /api/session. What an
// unauthenticated stranger who has already got onto the AP can learn from the
// device is exactly this and nothing else: what it is, what it is running, and
// that it wants a PIN.
//
// DELIBERATELY ABSENT: the module list, the MAC, the partition table, heap
// figures, session count, the SSID's parent MAC, and of course the PSK and PIN.
// If you are tempted to add a field here, add it to GET /api/modules instead.
//
// `pin_len` is the one deliberate addition (2026-08-24) and the distinction it
// draws is the one to apply to anything else proposed for this endpoint: the
// FORMAT of a credential is public — it is in this repository, in the protocol
// docs, and visible in the shape of every rejection message — while its VALUE
// is not. A client that needs to size an input box is asking about the format.
esp_err_t handleStatus(httpd_req_t *req) {
  const esp_app_desc_t *app = esp_app_get_description();
  char build[48];
  snprintf(build, sizeof(build), "%s %s", app->date, app->time);

  JsonDocument doc;
  doc["ok"] = true;
  JsonObject d = doc["d"].to<JsonObject>();
  d["name"] = (const char *)ssid_;
  d["fw"] = app->version;
  d["build"] = (const char *)build;
  d["auth_required"] = true;
  // The PIN's LENGTH, which is a published format constant, not a secret — the
  // value is the secret and is never here (see the DELIBERATELY ABSENT list
  // above). It is on this UNAUTHENTICATED endpoint on purpose: the pairing page
  // is what needs it, and the pairing page runs before there is any session to
  // authenticate with. Without it the UI has to hardcode 4, in an input's
  // maxlength and a validation branch, in a file that nothing recompiles when
  // AuthFmt::PIN_LEN moves — which is precisely how the wrong number survives a
  // change like the one that took this from 8 to 4.
  d["pin_len"] = (uint32_t)AuthFmt::PIN_LEN;
  d["api"] = 1;
  return sendJsonDoc(req, "200 OK", doc);
}

esp_err_t handleSessionCreate(httpd_req_t *req) {
  char body[MAX_SESSION_BODY];
  size_t len = 0;
  BodyResult br = readBody(req, body, sizeof(body), &len);
  if (br != BODY_OK) {
    return sendBodyError(req, br, sizeof(body));
  }

  JsonDocument reqDoc;
  DeserializationError perr = deserializeJson(reqDoc, body, len);
  if (perr) {
    memset(body, 0, sizeof(body));
    return sendErr(req, "400 Bad Request", "EPARSE", perr.c_str());
  }
  // COPY the candidate out before wiping the body buffer. ArduinoJson
  // deserialises a mutable char* IN PLACE and keeps pointers into it, so
  // clearing `body` first would leave `candidate` pointing at the zeros we just
  // wrote — and every PIN would then compare equal to "".
  //
  // The buffer is PIN_LEN + 2 and the comparison below uses PIN_LEN + 1, so a
  // candidate that merely STARTS with the right PIN_LEN digits — "12345" when
  // the PIN is "1234" — has a different length inside the compared window and
  // is rejected. Sizing either of those at PIN_LEN would accept it. Both are
  // derived from PIN_LEN and stay correct now that it is 4; nothing here
  // assumes a particular number of digits.
  char candidate[AuthFmt::PIN_LEN + 2] = {0};
  const char *pinIn = reqDoc["pin"] | (const char *)nullptr;
  bool havePin = pinIn != nullptr;
  if (havePin) {
    snprintf(candidate, sizeof(candidate), "%s", pinIn);
  }
  reqDoc.clear();
  memset(body, 0, sizeof(body));  // the PIN was in here

  if (!havePin) {
    // Deliberately NOT counted as a failed attempt and NOT rate-limited: a
    // request with no PIN in it has not guessed anything, so charging it an
    // attempt would let anyone lock the owner out with malformed requests.
    //
    // The example is BUILT from PIN_LEN, not written out. A literal "12345678"
    // sat here through the change to 4 digits and would have told every caller
    // hitting this path to send eight — an error message that teaches the wrong
    // format is worse than no error message. The count goes in too, so the
    // caller does not have to count the characters in the example.
    char example[AuthFmt::PIN_LEN + 1];
    for (size_t i = 0; i < AuthFmt::PIN_LEN; i++) {
      example[i] = (char)('1' + (i % 9));
    }
    example[AuthFmt::PIN_LEN] = '\0';
    char msg[128];
    snprintf(msg, sizeof(msg), "missing p.pin: POST {\"pin\":\"%s\"} — the PIN is a string of %u digits, not a number",
             example, (unsigned)AuthFmt::PIN_LEN);
    return sendErr(req, "400 Bad Request", "EARGS", msg);
  }

  uint32_t now = millis();
  RateLimit::Decision decision;
  uint32_t retryMs = 0;
  uint8_t attemptsLeft = 0;
  {
    Lock l(authLock_);
    decision = RateLimit::check(pinLimit_, now, &retryMs);
    attemptsLeft = RateLimit::remaining(pinLimit_);
  }
  if (decision != RateLimit::ALLOW) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s; retry in %u ms",
             decision == RateLimit::LOCKED ? "too many failed PIN attempts — locked out"
                                           : "too soon after a failed PIN attempt",
             (unsigned)retryMs);
    JsonDocument doc;
    doc["ok"] = false;
    JsonObject e = doc["e"].to<JsonObject>();
    e["code"] = decision == RateLimit::LOCKED ? "ELOCKED" : "ERATE";
    e["msg"] = (const char *)msg;
    JsonObject d = doc["d"].to<JsonObject>();
    d["retry_after_ms"] = retryMs;
    // The real figure, not a hardcoded 0: WAIT means the escalating delay is in
    // force and there ARE attempts left, and a caller that cannot tell WAIT
    // from LOCKED cannot tell a 1-second pause from a 15-minute one.
    d["attempts_remaining"] = attemptsLeft;
    char secs[12];
    snprintf(secs, sizeof(secs), "%u", (unsigned)((retryMs + 999) / 1000));
    httpd_resp_set_hdr(req, "Retry-After", secs);
    // attemptsLeft, not a hardcoded 0 — the SAME figure the body carries three
    // lines up, for the same reason. On LOCKED it is genuinely 0; on WAIT the
    // escalating delay is in force and there may be seven attempts left, and an
    // event saying 0 tells an operator watching http.auth that the device is
    // locked out when it is not. reportAttempts exists precisely so this field
    // is only emitted where it is true (see AuthEvt); emitting it with a
    // fabricated value defeats that.
    AuthEvt ev{decision == RateLimit::LOCKED ? "locked" : "rate_limited", attemptsLeft, retryMs, 0, true, false};
    Bus::emit("http.auth", fillAuthEvent, &ev);
    return sendJsonDoc(req, "429 Too Many Requests", doc);
  }

  bool ok;
  bool rotated = false;
  bool mintOnSuccess = false;
  {
    Lock l(authLock_);
    // Constant time, and it runs whatever the candidate looks like — an early
    // return on a wrong-length PIN is a timing signal too.
    ok = CT::equalStr(pin_, candidate, AuthFmt::PIN_LEN + 1);

    // ---- MINT TRIGGERS 2 AND 4, DECIDED IN ONE PLACE (pinpolicy.h) ------
    //
    // The limiter transition and the mint decision used to be open-coded here,
    // interleaved with the compare above and the session table below. They are
    // now one call, for one reason: the invariant they encode — THE LOCKOUT
    // MINTS A NEW PIN AND MUST NOT CLEAR THE LIMITER — is what makes 10^4
    // defensible (authfmt.h), and it could not be tested from this repository
    // while it lived in a .cpp the native env excludes. It has a host suite of
    // its own now (test_pinpolicy), which is the only coverage it can ever have
    // here: this handler is registered on the softAP listener and the build
    // machine has no radio to reach it with.
    //
    // afterAttempt() performs the limiter transition itself and returns what to
    // do about the PIN. Its precondition is satisfied above: check() returned
    // ALLOW, so this attempt is one the limiter agreed to count.
    PinPolicy::Outcome out = PinPolicy::afterAttempt(pinLimit_, ok, now);

    if (!ok && out.mint) {
      // Trigger 4: the tenth consecutive failure. The 15-minute lockout stands
      // — pinpolicy.h is what guarantees that, and its test asserts check()
      // still returns LOCKED for the full duration afterwards.
      //
      // A failed mint leaves the OLD PIN in place, which is the safe direction:
      // the lockout still stands, and the next trip (or enable, or session end)
      // mints again. Reported on the event rather than swallowed.
      char rerr[128];
      rotated = mintPinLocked(rerr, sizeof(rerr));
    }

    // Trigger 2 is DEFERRED, not skipped — see below. The policy decides
    // WHETHER the successful attempt spends the PIN; this handler decides WHEN,
    // and the answer is "once a session actually exists".
    mintOnSuccess = ok && out.mint;
  }

  if (!ok) {
    uint8_t left;
    uint32_t wait = 0;
    {
      Lock l(authLock_);
      left = RateLimit::remaining(pinLimit_);
      RateLimit::check(pinLimit_, now, &wait);
    }
    char msg[192];
    if (left == 0) {
      // The attempt that tripped the lockout. Saying "0 attempt(s) before a
      // 15-minute lockout" here would be true and useless — the lockout has
      // ALREADY started, and (unless the RNG failed) the PIN the caller was
      // typing no longer exists. A UI that does not learn the second half
      // leaves the user retyping a dead PIN for fifteen minutes.
      snprintf(msg, sizeof(msg), "incorrect PIN; locked out for %u minutes%s",
               (unsigned)(RateLimit::LOCKOUT_MS / 60000u),
               rotated ? " — and a NEW PIN is now on the device's screen" : "");
    } else {
      snprintf(msg, sizeof(msg), "incorrect PIN; %u attempt(s) before a %u-minute lockout", (unsigned)left,
               (unsigned)(RateLimit::LOCKOUT_MS / 60000u));
    }
    JsonDocument doc;
    doc["ok"] = false;
    JsonObject e = doc["e"].to<JsonObject>();
    e["code"] = "EPIN";
    e["msg"] = (const char *)msg;
    JsonObject d = doc["d"].to<JsonObject>();
    d["attempts_remaining"] = left;
    d["retry_after_ms"] = wait;
    if (rotated) {
      // The fact, never the value. This is what tells the page to stop offering
      // the PIN the user just typed and send them back to the LCD.
      d["pin_rotated"] = true;
    }
    // Logged as an event, not a Serial line: stdout is the JSON-lines console
    // protocol. No secret, no candidate PIN, not even its length.
    AuthEvt ev{"fail", left, wait, 0, true, rotated};
    Bus::emit("http.auth", fillAuthEvent, &ev);
    memset(candidate, 0, sizeof(candidate));
    return sendJsonDoc(req, "401 Unauthorized", doc);
  }
  memset(candidate, 0, sizeof(candidate));

  char token[AuthFmt::TOKEN_LEN + 1];
  bool evicted = false;
  uint32_t id = createSession(token, sizeof(token), now, &evicted);
  if (id == 0) {
    return sendErr(req, "500 Internal Server Error", "ERNG",
                   "could not generate a session token; no session was created");
  }

  // ---- MINT TRIGGER 2 OF 4: THE PIN IS CONSUMED BY USE ------------------
  //
  // `mintOnSuccess` is PinPolicy::afterAttempt()'s instruction, carried down
  // from the compare above. WHY IT IS CARRIED RATHER THAN ACTED ON THERE: the
  // PIN must not be spent for a session that does not exist. createSession()
  // can fail (an RNG failure minting the token), and rotating before that is
  // known would leave the caller holding a dead PIN, no session, and a 500 —
  // recoverable only by walking over to read the LCD again. The policy decides
  // whether pairing spends the PIN; this is the point at which pairing has
  // actually happened.
  //
  // Stuart's model is a "4-digit pin code (single use)", and this is the line
  // that makes "single use" true rather than aspirational. The PIN that was
  // just exchanged for a token is spent; from here it must never open a session
  // again.
  //
  // IT IS A SESSION-TAKEOVER HOLE OTHERWISE, not untidiness. MAX_SESSIONS is 1
  // and createSession() EVICTS rather than refusing, so a still-valid PIN is a
  // credential that displaces the rightful owner's session — anyone who read
  // the digits off the LCD before the owner paired could come back at any point
  // inside SESSION_IDLE_MS and take the device. Rotating here closes it: the
  // window in which a PIN is worth anything ends the moment it is used.
  //
  // AND IT MATTERS MORE ONCE THE PIN IS IN A QR CODE. The pair QR carries the
  // PIN in the URL (ARCHITECTURE.md, "QR pairing on the LCD"), so it is
  // photographable off the panel from across a room and survives in a camera
  // roll. Consumption is what makes that photograph worthless the instant the
  // owner pairs; the page's history.replaceState() only stops a RELOAD from
  // re-pairing, which is a different and much smaller problem.
  //
  // ORDERING, checked rather than assumed:
  //   * The token in `token` was minted by createSession() from esp_random()
  //     and has no relationship to the PIN, so rotating now cannot affect what
  //     goes back in the response body below.
  //   * The value minted here is NOT reachable by this caller. It never enters
  //     a response, and httpTick()'s Pairing::shouldShow(apUp_, live) is false
  //     while a session is live, so the tick withdraws from the LCD instead of
  //     publishing it. It sits in RAM until the session ends, at which point
  //     trigger 3 or 4 replaces it again anyway.
  //   * A failed mint leaves the OLD PIN live. That is the pre-existing
  //     behaviour rather than a new hole, it is reported as `pin_rotated:
  //     false`, and the next enable/lockout/session-end mints again.
  //
  // Named apart from the `rotated` the lockout path uses, deliberately: that
  // one belongs to a failed attempt and this one to a successful pairing, and
  // reusing the variable would make two unrelated outcomes look like one.
  bool pinConsumed = false;
  if (mintOnSuccess) {
    Lock l(authLock_);
    char rerr[128];
    pinConsumed = mintPinLocked(rerr, sizeof(rerr));
  }

  JsonDocument doc;
  doc["ok"] = true;
  JsonObject d = doc["d"].to<JsonObject>();
  // The one and only place a token is ever written to the network — and the
  // response that grants it, by definition at AUTH_TOKEN.
  d["token"] = (const char *)token;
  d["expires_in"] = (uint32_t)(SESSION_IDLE_MS / 1000u);
  d["lifetime_s"] = (uint32_t)(SESSION_ABSOLUTE_MS / 1000u);
  d["session"] = id;
  if (evicted) {
    d["evicted_oldest"] = true;
  }
  // The fact, never the value — and the caller has just spent the PIN it sent,
  // so a UI holding it (from a typed field or a scanned QR) knows to discard it
  // rather than offer it again.
  d["pin_rotated"] = pinConsumed;
  AuthEvt ev{"ok", 0, 0, id, false, pinConsumed};
  Bus::emit("http.auth", fillAuthEvent, &ev);
  esp_err_t r = sendJsonDoc(req, "200 OK", doc);
  memset(token, 0, sizeof(token));
  return r;
}

// ---- MINT TRIGGER 3 OF 4, THE OTHER HALF: EXPLICIT UNPAIR ---------------
//
// The user pressing "unpair" in the web UI. ARCHITECTURE.md defines session end
// as "90 s after the single AP client disassociates, OR an explicit unpair",
// and design/BRIEF.md 4.2 promises the user that unpairing "revokes the session
// and puts a new PIN on the device's screen". Revoking without minting would
// republish the OLD PIN — the one this very caller just spent to pair — and
// silently break that promise while looking like it kept it.
//
// Only an ACTUAL revocation rotates. `was_live` false means the token
// authenticated but its session had already gone (reaped between the
// authenticate() above and the revoke), so there was no pairing to end.
esp_err_t handleSessionDelete(httpd_req_t *req) {
  uint32_t id = authenticate(req);
  if (id == 0) {
    return send401(req);
  }
  bool gone = revokeSession(id);
  // `was_live` false means the token authenticated but its session had already
  // gone — reaped between the authenticate() above and the revoke. Nothing
  // ended, so nothing rotates; rotateAfterRevocation() enforces that from the
  // count rather than trusting this call site to.
  bool rotated = rotateAfterRevocation(gone ? 1 : 0, id);
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject d = doc["d"].to<JsonObject>();
  d["revoked"] = id;
  d["was_live"] = gone;
  // So the page can say "a new PIN is on the screen" rather than guessing, and
  // so it knows to discard anything it was holding. The fact, never the value.
  d["pin_rotated"] = rotated;
  return sendJsonDoc(req, "200 OK", doc);
}

esp_err_t handleModules(httpd_req_t *req) {
  if (authenticate(req) == 0) {
    return send401(req);
  }
  if (!enterRegistry()) {
    return sendErr(req, "503 Service Unavailable", "ESTOPPING", "the Wi-Fi transport is shutting down");
  }
  JsonDocument doc;
  doc["ok"] = true;
  // Identical to the console's `modules` command, because it IS the console's
  // `modules` command (console.h). The registry takes and releases its own lock
  // inside this call; the response is fully built before a byte is sent, so no
  // registry lock is ever held across a network write.
  // AUTH_TOKEN, matching handleCmd(): authenticate() above proves a session and
  // nothing more. Never AUTH_PHYSICAL — that level means "is holding the
  // cable", which no network client ever is. It is what filters the listing
  // (backlog S7) and sets each action's `allowed`.
  Console::fillModules(doc["d"].to<JsonObject>(), AUTH_TOKEN);
  exitRegistry();
  return sendJsonDoc(req, "200 OK", doc);
}

// ---- THE 401 IS NOW DEFENCE IN DEPTH, NOT THE DEFENCE ---------------------
//
// It used to be the only thing standing in front of the built-in commands:
// `info`, `parts`, `mem`, `bootprobe` and `reboot` were dispatched from
// console.cpp's table without ever reading ctx.authLevel, so this handler
// returning 401 before Console::execute() WAS the security boundary for one
// third of the command surface — in a design whose premise is that the command
// layer decides for itself, precisely so that three transport adapters cannot
// disagree about it.
//
// Fixed by backlog S1 (2026-08-17): every built-in carries a minimum AuthLevel
// (cmdauth.h) which Console::execute() enforces centrally. A TRANSPORT MAY NOW
// DISPATCH AT AUTH_NONE SAFELY — an unauthenticated caller gets EAUTH from the
// command layer, not a partition table. That is the property the BLE adapter
// (backlog F1) is meant to rely on, and it is why this stays: two independent
// refusals, neither of which is load-bearing alone.
//
// Keeping it also means an unauthenticated HTTP client is refused by HTTP's own
// vocabulary — 401 with WWW-Authenticate, which is what a browser and a curl
// user expect — rather than a 200 carrying an EAUTH envelope.
esp_err_t handleCmd(httpd_req_t *req) {
  if (authenticate(req) == 0) {
    return send401(req);
  }
  if (rxBuf_ == nullptr) {
    return sendErr(req, "503 Service Unavailable", "ENOBUF", "the request buffer is not allocated");
  }

  size_t len = 0;
  BodyResult br = readBody(req, rxBuf_, MAX_BODY + 1, &len);
  if (br != BODY_OK) {
    return sendBodyError(req, br, MAX_BODY + 1);
  }

  JsonDocument reqDoc;
  DeserializationError perr = deserializeJson(reqDoc, rxBuf_, len);
  if (perr) {
    return sendErr(req, "400 Bad Request", "EPARSE", perr.c_str());
  }

  if (!enterRegistry()) {
    return sendErr(req, "503 Service Unavailable", "ESTOPPING", "the Wi-Fi transport is shutting down");
  }
  // AUTH_TOKEN, never AUTH_PHYSICAL: that level means "is holding the cable",
  // and nothing arriving over a radio can prove it.
  JsonDocument resp;
  bool restart = Console::execute(reqDoc.as<JsonObjectConst>(), AUTH_TOKEN, "http", resp);
  exitRegistry();
  esp_err_t r = sendJsonDoc(req, "200 OK", resp);
  if (restart) {
    // Deferred to the module tick so this handler can return and the socket can
    // actually flush. Rebooting from inside the handler drops the response.
    // Deadline and reason BEFORE the flag — see the declaration.
    snprintf(rebootReason_, sizeof(rebootReason_), "http");
    rebootAtMs_ = millis() + REBOOT_DELAY_MS;
    rebootPending_.store(true, std::memory_order_release);
  }
  return r;
}

// ---- WebSocket -----------------------------------------------------------
//
// AUTHENTICATION, documented because the brief left the choice open:
//
//   1. `Authorization: Bearer <token>` on the UPGRADE request. Works for curl
//      and native clients and is the cleanest form — but browsers cannot set
//      headers on a WebSocket handshake, so it cannot be the only way.
//   2. A FIRST-MESSAGE auth frame: {"id":1,"act":"auth","p":{"token":"..."}}.
//      This is what the embedded page uses.
//
// Both are accepted. What is NOT accepted is a token in the query string:
// URLs land in proxy logs, browser history and Referer headers, and a session
// token has no business in any of them.
//
// Until a socket is authenticated the ONLY frame it may send is that auth
// frame. Anything else is answered with EAUTH and the socket is closed — so an
// unauthenticated client can reach the command bus at no level, and the
// AUTH_NONE CmdContext never gets as far as a dispatch.
//
// That last sentence is now belt AND braces rather than the belt: since backlog
// S1 the built-ins enforce their own minimum AuthLevel inside
// Console::execute() (cmdauth.h), so dispatching an AUTH_NONE context would be
// safe. This transport still refuses first — see the block above handleCmd() —
// because closing the socket is a cheaper answer to a stranger than parsing and
// dispatching their command.
esp_err_t handleWs(httpd_req_t *req) {
  int fd = httpd_req_to_sockfd(req);
  uint32_t now = millis();

  if (req->method == HTTP_GET) {
    // Handshake already completed by esp_http_server; this is our chance to
    // record the client and pick up a header credential.
    uint32_t id = authenticate(req);
    if (!wsAdd(fd, id != 0, id, now)) {
      char msg[128];
      snprintf(msg, sizeof(msg), "{\"ok\":false,\"e\":{\"code\":\"EMAXWS\",\"msg\":\"too many WebSocket clients (%u)\"}}",
               (unsigned)MAX_WS_CLIENTS);
      wsSendText(fd, msg, strlen(msg));
      httpd_sess_trigger_close(server_, fd);
    }
    return ESP_OK;
  }

  if (rxBuf_ == nullptr) {
    return ESP_FAIL;
  }

  httpd_ws_frame_t f;
  memset(&f, 0, sizeof(f));
  f.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t err = httpd_ws_recv_frame(req, &f, 0);  // length only
  if (err != ESP_OK) {
    return err;
  }
  if (f.type == HTTPD_WS_TYPE_CLOSE) {
    // Belt and braces: with handle_ws_control_frames false, esp_http_server
    // deals with CLOSE itself and this handler is not called for it. The slot
    // is freed by onSocketClose() either way, and both paths are idempotent.
    wsRemove(fd);
    return ESP_OK;
  }
  if (f.len == 0) {
    return ESP_OK;
  }
  if (f.len > MAX_WS_RX) {
    // The payload cannot be skipped without reading it, and reading it is
    // precisely what the bound forbids — so the socket is closed rather than
    // left desynchronised mid-frame. Same limit as CDC (Protocol::MAX_LINE), so
    // a request that works on one transport works on the other.
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"ok\":false,\"e\":{\"code\":\"ELINE\",\"msg\":\"frame is %u bytes; the limit is %u — closing\"}}",
             (unsigned)f.len, (unsigned)MAX_WS_RX);
    wsSendText(fd, msg, strlen(msg));
    wsRemove(fd);
    httpd_sess_trigger_close(server_, fd);
    return ESP_OK;
  }

  f.payload = (uint8_t *)rxBuf_;
  err = httpd_ws_recv_frame(req, &f, MAX_WS_RX);
  if (err != ESP_OK) {
    return err;
  }
  if (f.type != HTTPD_WS_TYPE_TEXT) {
    // Binary frames carry no protocol here. So does a CONTINUATION frame: this
    // transport does NOT reassemble a fragmented message, deliberately — one
    // frame is one protocol line, matching the CDC transport's one-line model,
    // and browsers do not fragment messages of this size. A client that does
    // will see its continuation dropped rather than misparsed.
    return ESP_OK;
  }
  rxBuf_[f.len] = '\0';

  JsonDocument reqDoc;
  DeserializationError perr = deserializeJson(reqDoc, rxBuf_, f.len);
  if (perr) {
    JsonDocument doc;
    doc["ok"] = false;
    JsonObject e = doc["e"].to<JsonObject>();
    e["code"] = "EPARSE";
    e["msg"] = perr.c_str();
    wsSendDoc(fd, doc);
    return ESP_OK;
  }
  JsonObjectConst request = reqDoc.as<JsonObjectConst>();
  const char *act = request["act"] | (const char *)nullptr;

  // The transport-scoped `auth` frame. It is not a bus command and never
  // reaches the registry — the envelope is the same only so a client has one
  // parser, exactly as the CDC bare-word shim reuses the envelope.
  if (act != nullptr && strcmp(act, "auth") == 0) {
    const char *tok = request["p"]["token"] | (const char *)nullptr;
    uint32_t id = touchSession(tok, now);
    JsonDocument doc;
    if (!request["id"].isNull()) {
      doc["id"] = request["id"];
    }
    if (id == 0) {
      doc["ok"] = false;
      JsonObject e = doc["e"].to<JsonObject>();
      e["code"] = "EAUTH";
      e["msg"] = "invalid or expired session token; POST /api/session with the device PIN to get one";
      wsSendDoc(fd, doc);
      wsRemove(fd);
      httpd_sess_trigger_close(server_, fd);
      return ESP_OK;
    }
    wsSetAuthed(fd, id);
    doc["ok"] = true;
    JsonObject d = doc["d"].to<JsonObject>();
    d["auth"] = "token";
    d["session"] = id;
    d["expires_in"] = (uint32_t)(SESSION_IDLE_MS / 1000u);
    d["max_line"] = (uint32_t)MAX_WS_RX;
    wsSendDoc(fd, doc);
    return ESP_OK;
  }

  // EVERY command frame, not just the first: the socket's session must still be
  // live right now. See the block above wsSessionStillLive() for what this
  // replaced and why a one-shot flag was the wrong question. It also refreshes
  // the session's idle timer, which is what stops a WS-only client being reaped
  // mid-use.
  if (!wsSessionStillLive(fd, now)) {
    JsonDocument doc;
    if (!request["id"].isNull()) {
      doc["id"] = request["id"];
    }
    doc["ok"] = false;
    JsonObject e = doc["e"].to<JsonObject>();
    e["code"] = "EAUTH";
    // One message for two causes — never authenticated, and authenticated
    // under a session that has since ended — because the remedy is identical
    // and the difference is not the client's business. A socket that WAS
    // authenticated is told plainly that its session ended rather than that it
    // never authenticated, which is the difference between "re-pair" and "your
    // code is broken".
    e["msg"] =
        "this socket has no live session: it never authenticated, or its session was revoked, expired or "
        "unpaired. Re-pair and send {\"act\":\"auth\",\"p\":{\"token\":\"...\"}}";
    wsSendDoc(fd, doc);
    // Out of the table BEFORE the close is triggered.
    // httpd_sess_trigger_close() only QUEUES the close onto the server task, so
    // until that runs the socket still exists — and a slot left in place would
    // still be `authed` for the event fan-out's next pass. wsRemove() clears
    // the whole entry, so both the fan-out and any further frame on this fd see
    // an unknown socket.
    wsRemove(fd);
    httpd_sess_trigger_close(server_, fd);
    return ESP_OK;
  }

  if (!enterRegistry()) {
    JsonDocument doc;
    if (!request["id"].isNull()) {
      doc["id"] = request["id"];
    }
    doc["ok"] = false;
    JsonObject e = doc["e"].to<JsonObject>();
    e["code"] = "ESTOPPING";
    e["msg"] = "the Wi-Fi transport is shutting down";
    wsSendDoc(fd, doc);
    return ESP_OK;
  }
  JsonDocument resp;
  bool restart = Console::execute(request, AUTH_TOKEN, "ws", resp);
  exitRegistry();
  wsSendDoc(fd, resp);
  if (restart) {
    snprintf(rebootReason_, sizeof(rebootReason_), "ws");
    rebootAtMs_ = millis() + REBOOT_DELAY_MS;
    rebootPending_.store(true, std::memory_order_release);
  }
  return ESP_OK;
}

// ===========================================================================
// POST /api/ota — the OTA DELIVERY path (backlog S5)
// ===========================================================================
//
// ---- THE TRADE-OFF, RECORDED ONCE, HERE -----------------------------------
//
// This endpoint is AUTH_TOKEN. A phone session may push a firmware image into
// the inactive slot AND select it as the next boot partition. Stuart chose this
// deliberately on 2026-08-17, having been told plainly what follows:
//
//   * A SESSION TOKEN IS NOW EQUIVALENT TO ARBITRARY CODE EXECUTION ON THIS
//     DEVICE. Whoever holds one can replace the firmware with anything that
//     passes esp_ota_end()'s image validation, select it, and wait for the next
//     restart. Everything else the auth model protects — the AP passphrase at
//     AUTH_PHYSICAL, `reboot` at AUTH_PHYSICAL, the PIN on the LCD — is
//     downstream of code that this endpoint can replace.
//   * COMBINED WITH THE PUBLIC AP PASSPHRASE (backlog S3, `pass-a9d8`,
//     derivable from the broadcast SSID), THE SECURITY BOUNDARY OF THIS DEVICE
//     IS RADIO RANGE. WPA2-PSK has no forward secrecy against a holder of the
//     key, so a captured handshake yields the PIN and the token too.
//
// That is the decision, it was informed, and it is not to be re-argued in code:
// no extra gate, no runtime nagging. `ota confirm` / `rollback` / `boot` remain
// AUTH_PHYSICAL — this is a NEW path, not a widening of those.
//
// The mitigations that do remain, unchanged: the PIN rate limiter, the session
// lifetimes, the AP never being default-enabled, and the fact that this never
// reboots — the new image does not run until someone restarts the device.
//
// ---- WHAT IS BOUNDED AND WHERE --------------------------------------------
//
// The size cap, the slot cap, the buffer, the stall and total timeouts and the
// abort discipline are all in otaupload.cpp. This handler owns only the parsing
// of the request and the shape of the response.
//
// ---- WHY IT PARTICIPATES IN THE SHUTDOWN HANDSHAKE ------------------------
//
// enterRegistry() has nothing to do with the registry here, and everything to
// do with what its counter actually means: "a handler is in flight on the HTTP
// task that httpd_stop() would have to wait for". A transfer can run for
// minutes. Without the increment, `disable http` would find inRegistry_ == 0,
// call httpd_stop() INLINE with the registry lock held, and join a task that is
// busy uploading — freezing the loop task for the rest of the transfer. With
// it, the teardown defers to httpTransportPoll(), and the reader below returns
// READ_ERROR the moment `stopping_` is set, so the upload aborts cleanly within
// one recv timeout (5 s).
constexpr size_t MAX_OTA_QUERY = 160;

struct OtaReadCtx {
  httpd_req_t *req;
};

OtaUpload::ReadStatus otaRead(void *ctx, uint8_t *buf, size_t cap, size_t *got) {
  *got = 0;
  OtaReadCtx *c = (OtaReadCtx *)ctx;
  if (stopping_.load(std::memory_order_acquire)) {
    // The transport is going down. Give up now rather than holding the teardown
    // for the rest of the transfer; otaupload.cpp aborts the slot.
    return OtaUpload::READ_ERROR;
  }
  int n = httpd_req_recv(c->req, (char *)buf, cap);
  if (n > 0) {
    *got = (size_t)n;
    return OtaUpload::READ_DATA;
  }
  if (n == HTTPD_SOCK_ERR_TIMEOUT) {
    return OtaUpload::READ_TIMEOUT;
  }
  if (n == 0) {
    return OtaUpload::READ_EOF;
  }
  return OtaUpload::READ_ERROR;
}

// 64 hex characters -> 32 bytes. Rejects anything else, including the uppercase
// form's mixed cousins — sha256sum(1) emits lowercase and so does this device's
// own response, so accepting both cases would give a digest two spellings.
bool parseSha256(const char *s, uint8_t *out) {
  if (s == nullptr || strlen(s) != 64) {
    return false;
  }
  for (uint8_t i = 0; i < 32; i++) {
    uint8_t v = 0;
    for (uint8_t half = 0; half < 2; half++) {
      char c = s[i * 2 + half];
      uint8_t nib;
      if (c >= '0' && c <= '9') {
        nib = (uint8_t)(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        nib = (uint8_t)(c - 'a' + 10);
      } else {
        return false;
      }
      v = (uint8_t)((v << 4) | nib);
    }
    out[i] = v;
  }
  return true;
}

// Sends an error and then makes esp_http_server CLOSE the socket.
//
// Not decoration. When a handler returns ESP_OK with request body still
// unread, httpd_req_delete() drains it — for a rejected 1.25 MB upload that is
// megabytes read and thrown away on the server task, after we have already
// answered. Returning ESP_FAIL after the response is on the wire closes the
// connection instead. The client still gets its status and error object.
esp_err_t otaReject(httpd_req_t *req, const char *status, const char *code, const char *msg) {
  sendErr(req, status, code, msg);
  return ESP_FAIL;
}

esp_err_t handleOta(httpd_req_t *req) {
  if (authenticate(req) == 0) {
    // No body has been read, and a stranger's is not going to be drained on our
    // task either.
    send401(req);
    return ESP_FAIL;
  }

  // ---- the query -------------------------------------------------------
  //
  // len is REQUIRED and is the caller's declaration of the image size. It is
  // checked against Content-Length as well, so a mismatch between what the
  // client says and what its HTTP stack says is caught before a slot is erased.
  char query[MAX_OTA_QUERY];
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0) {
    return otaReject(req, "400 Bad Request", "EARGS",
                     "POST /api/ota?len=<bytes>[&sha256=<64 hex>][&select=1] — the image size must be declared");
  }
  if (qlen >= sizeof(query)) {
    return otaReject(req, "400 Bad Request", "EARGS", "the query string is too long");
  }
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return otaReject(req, "400 Bad Request", "EARGS", "the query string could not be read");
  }

  OtaUpload::Params params;
  char val[72];
  if (httpd_query_key_value(query, "len", val, sizeof(val)) != ESP_OK) {
    return otaReject(req, "400 Bad Request", "EARGS",
                     "missing ?len=<bytes>: the image size must be declared up front");
  }
  char *end = nullptr;
  unsigned long declared = strtoul(val, &end, 10);
  if (end == val || *end != '\0' || declared == 0) {
    return otaReject(req, "400 Bad Request", "EARGS", "?len must be a positive decimal byte count");
  }
  params.declaredLen = (uint32_t)declared;

  if (httpd_query_key_value(query, "sha256", val, sizeof(val)) == ESP_OK) {
    if (!parseSha256(val, params.sha)) {
      return otaReject(req, "400 Bad Request", "EARGS",
                       "?sha256 must be exactly 64 lowercase hex characters (the output of sha256sum firmware.bin)");
    }
    params.haveSha = true;
  }
  if (httpd_query_key_value(query, "select", val, sizeof(val)) == ESP_OK) {
    // EXPLICIT, and only these spellings. "Select the image I just uploaded" is
    // the one irreversible-ish half of this endpoint, so it is never inferred
    // from the presence of the parameter alone.
    if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0) {
      params.select = true;
    } else if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0) {
      params.select = false;
    } else {
      return otaReject(req, "400 Bad Request", "EARGS", "?select must be 1/true or 0/false");
    }
  }

  // ---- the body's length, before anything is erased --------------------
  if (req->content_len == 0) {
    // Includes chunked transfer-encoding, which esp_http_server reports as a
    // zero content_len. Refused by name rather than read until it stops.
    return otaReject(req, "411 Length Required", "ELENGTH",
                     "a Content-Length is required (chunked transfer-encoding is not accepted here)");
  }
  if (req->content_len != params.declaredLen) {
    char msg[144];
    snprintf(msg, sizeof(msg), "?len is %u but Content-Length is %u; they must agree", (unsigned)params.declaredLen,
             (unsigned)req->content_len);
    return otaReject(req, "400 Bad Request", "ELENGTH", msg);
  }

  if (!enterRegistry()) {
    return otaReject(req, "503 Service Unavailable", "ESTOPPING", "the Wi-Fi transport is shutting down");
  }

  OtaReadCtx rctx = {req};
  OtaUpload::Report rep;
  OtaUpload::run(params, otaRead, &rctx, rep);
  exitRegistry();

  JsonDocument doc;
  doc["ok"] = rep.ok;
  if (!rep.ok) {
    JsonObject e = doc["e"].to<JsonObject>();
    e["code"] = rep.code != nullptr ? rep.code : "EFAIL";
    e["msg"] = (const char *)rep.msg;
  }
  // `d` on BOTH paths (ARCHITECTURE.md section 2): a failed upload still has to
  // say how far it got, which slot it touched and what the device will boot.
  OtaUpload::fillReport(doc["d"].to<JsonObject>(), rep);

  const char *status = "200 OK";
  switch (rep.httpStatus) {
    case 400:
      status = "400 Bad Request";
      break;
    case 408:
      status = "408 Request Timeout";
      break;
    case 409:
      status = "409 Conflict";
      break;
    case 411:
      status = "411 Length Required";
      break;
    case 413:
      status = "413 Payload Too Large";
      break;
    case 500:
      status = "500 Internal Server Error";
      break;
    case 503:
      status = "503 Service Unavailable";
      break;
    default:
      break;
  }
  esp_err_t sent = sendJsonDoc(req, status, doc);
  if (!rep.ok) {
    // Same reason as otaReject(): on ESHORT / ECONN / ETIMEOUT there may still
    // be megabytes of body queued, and draining it after we have answered would
    // occupy the server task for as long again as the failed upload did.
    return ESP_FAIL;
  }
  return sent;
}

// Called by esp_http_server for EVERY socket it closes, WebSocket or not, on
// its own task. Overriding it means we own the close(), which the default
// implementation would otherwise do.
void onSocketClose(httpd_handle_t hd, int sockfd) {
  (void)hd;
  wsRemove(sockfd);
  close(sockfd);
}

// ===========================================================================
// Lifecycle
// ===========================================================================

char enableErr_[192];

// The ONE place the AP's parameters are spelled out, so httpEnable() and the
// `psk set` restart cannot drift apart.
//
// WPA2-PSK + CCMP are this framework's defaults for softAP with a passphrase
// (WIFI_AP_DEFAULT_AUTH_MODE / WIFI_AP_DEFAULT_CIPHER, WiFiAP.h:33) — passed
// explicitly anyway so a framework default cannot silently downgrade us to
// WPA/WPA2-mixed with TKIP. ftm_responder stays false. WPS is never enabled:
// nothing here calls esp_wifi_ap_wps_enable(), and it is off by default.
bool startAp(const char *passphrase) {
  return WiFi.softAP(ssid_, passphrase, AP_CHANNEL, /*ssid_hidden=*/0, AP_MAX_CLIENTS, /*ftm_responder=*/false,
                     WIFI_AUTH_WPA2_PSK, WIFI_CIPHER_TYPE_CCMP);
}

bool startServer(char *err, size_t errCap) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  // The default 4096 is not enough, and this is the number to watch on
  // hardware. A handler on this task runs the WHOLE command surface, not just
  // HTTP: ArduinoJson, then Console::execute(), then any module's dispatch —
  // and `storage.delete` recurses DELETE_MAX_DEPTH (8) levels at ~200 bytes a
  // frame. 10 KB of internal RAM, allocated by httpd_start() and freed by
  // httpd_stop(), so it costs nothing while the module is off.
  // TEST FOR: uxTaskGetStackHighWaterMark on this task after a deep
  // storage.delete over /api/cmd.
  cfg.stack_size = 10240;
  cfg.max_open_sockets = MAX_OPEN_SOCKETS;
  cfg.max_uri_handlers = 8;
  cfg.lru_purge_enable = true;  // a stuck client must not hold a socket forever
  cfg.recv_wait_timeout = 5;
  cfg.send_wait_timeout = 5;
  cfg.close_fn = onSocketClose;
  // Everything else is left at HTTPD_DEFAULT_CONFIG, including task_priority
  // (tskIDLE_PRIORITY+5) — see the threading note at the top.

  esp_err_t e = httpd_start(&server_, &cfg);
  if (e != ESP_OK) {
    server_ = nullptr;
    snprintf(err, errCap, "httpd_start failed: %s", esp_err_to_name(e));
    return false;
  }

  static const httpd_uri_t URIS[] = {
      {.uri = "/", .method = HTTP_GET, .handler = handlePage, .user_ctx = nullptr,
       .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr},
      {.uri = "/api/status", .method = HTTP_GET, .handler = handleStatus, .user_ctx = nullptr,
       .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr},
      {.uri = "/api/session", .method = HTTP_POST, .handler = handleSessionCreate, .user_ctx = nullptr,
       .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr},
      {.uri = "/api/session", .method = HTTP_DELETE, .handler = handleSessionDelete, .user_ctx = nullptr,
       .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr},
      {.uri = "/api/modules", .method = HTTP_GET, .handler = handleModules, .user_ctx = nullptr,
       .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr},
      {.uri = "/api/cmd", .method = HTTP_POST, .handler = handleCmd, .user_ctx = nullptr,
       .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr},
      // The OTA delivery path (backlog S5). NOT a command-bus action: it
      // streams a 1.25 MB body straight into flash, which no JSON envelope
      // should ever carry. See the block above handleOta().
      {.uri = "/api/ota", .method = HTTP_POST, .handler = handleOta, .user_ctx = nullptr,
       .is_websocket = false, .handle_ws_control_frames = false, .supported_subprotocol = nullptr},
      // handle_ws_control_frames stays false: esp_http_server answers a client
      // PING with a PONG itself, on the same task, so no send can interleave.
      {.uri = "/ws", .method = HTTP_GET, .handler = handleWs, .user_ctx = nullptr,
       .is_websocket = true, .handle_ws_control_frames = false, .supported_subprotocol = nullptr},
  };
  static_assert(sizeof(URIS) / sizeof(URIS[0]) <= 8, "max_uri_handlers must cover every registered URI");

  for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
    e = httpd_register_uri_handler(server_, &URIS[i]);
    if (e != ESP_OK) {
      snprintf(err, errCap, "httpd_register_uri_handler(%s) failed: %s", URIS[i].uri, esp_err_to_name(e));
      httpd_stop(server_);
      server_ = nullptr;
      return false;
    }
  }
  return true;
}

bool httpEnable(const char **errMsg) {
  enableErr_[0] = '\0';

  if (teardownPending_.load(std::memory_order_acquire)) {
    // A previous disable could not stop the server synchronously (a request was
    // in flight) and the deferred teardown has not run yet. Starting a second
    // AP and server on top of the one being torn down is not a race worth
    // having, and the wait is one scheduler pass.
    snprintf(enableErr_, sizeof(enableErr_),
             "a previous disable is still tearing the server down (a request was in flight); retry in a moment");
    *errMsg = enableErr_;
    return false;
  }

  if (authLock_ == nullptr) {
    authLock_ = xSemaphoreCreateMutex();
    wsLock_ = xSemaphoreCreateMutex();
    ringLock_ = xSemaphoreCreateMutex();
  }
  if (authLock_ == nullptr || wsLock_ == nullptr || ringLock_ == nullptr) {
    snprintf(enableErr_, sizeof(enableErr_), "could not create the transport's mutexes (out of memory)");
    *errMsg = enableErr_;
    return false;
  }

  buildSsid();

  // BEFORE the radio comes up — see the entropy note. A failure here is fatal
  // to enable(), and what makes it fatal is now the PASSPHRASE alone: bringing
  // an AP up on a key we could not persist would mean the WPA2 credential the
  // owner just wrote down stops working at the next boot. The PIN changing at
  // the next boot is not a failure any more, it is the design — it changes at
  // every enable by construction, and loadOrCreateCredentials() will not refuse
  // to start over it.
  if (!loadOrCreateCredentials(enableErr_, sizeof(enableErr_))) {
    *errMsg = enableErr_;
    return false;
  }

  rxBuf_ = (char *)malloc(MAX_WS_RX + 1);
  ring_ = (EventSlot *)malloc(sizeof(EventSlot) * EVENT_SLOTS);
  if (rxBuf_ == nullptr || ring_ == nullptr) {
    free(rxBuf_);
    free(ring_);
    rxBuf_ = nullptr;
    ring_ = nullptr;
    snprintf(enableErr_, sizeof(enableErr_), "out of heap for the %u-byte request buffer and %u-byte event ring",
             (unsigned)(MAX_WS_RX + 1), (unsigned)(sizeof(EventSlot) * EVENT_SLOTS));
    *errMsg = enableErr_;
    return false;
  }
  ringHead_ = ringTail_ = ringCount_ = 0;

  if (!WiFi.mode(WIFI_AP)) {
    free(rxBuf_);
    free(ring_);
    rxBuf_ = nullptr;
    ring_ = nullptr;
    snprintf(enableErr_, sizeof(enableErr_), "WiFi.mode(WIFI_AP) failed; the radio would not start");
    *errMsg = enableErr_;
    return false;
  }
  if (!startAp(psk_)) {
    WiFi.mode(WIFI_OFF);
    free(rxBuf_);
    free(ring_);
    rxBuf_ = nullptr;
    ring_ = nullptr;
    snprintf(enableErr_, sizeof(enableErr_), "softAP(\"%s\") failed; the AP did not start", ssid_);
    *errMsg = enableErr_;
    return false;
  }
  apUp_ = true;

  if (!startServer(enableErr_, sizeof(enableErr_))) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apUp_ = false;
    free(rxBuf_);
    free(ring_);
    rxBuf_ = nullptr;
    ring_ = nullptr;
    *errMsg = enableErr_;
    return false;
  }

  // Registered once for the life of the image: Bus has no removeSink() (see
  // bus.h), so the sink stays and goes quiet when ring_ is null.
  if (!sinkRegistered_) {
    sinkRegistered_ = Bus::addSink(eventSink);
  }

  // Deliberately NOT logging the SSID's credentials, and deliberately not a
  // bare Serial line either — stdout is the JSON-lines protocol. The `status`
  // action reports the SSID and IP; `psk` and `pin` report the secrets, at
  // AUTH_PHYSICAL only.
  return true;
}

// The real teardown. Runs EITHER inline from httpDisable() (when the Dekker
// handshake proved no handler can be inside a registry call) or from
// httpTransportPoll(), which is a plain scheduler task and therefore holds no
// registry lock. It must never run with the registry lock held AND a handler
// in flight — that is the deadlock the handshake exists to detect.
void teardownNow() {
  // ORDER MATTERS. httpd_stop() joins the server task, so once it returns no
  // handler and no queued work item can be running — which is what makes it
  // safe to free rxBuf_ and ring_ below. Freeing first would be a
  // use-after-free that only ever appears under load.
  if (server_ != nullptr) {
    httpd_stop(server_);
    server_ = nullptr;
  }
  if (apUp_) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apUp_ = false;
  }

  {
    Lock l(ringLock_);
    free(ring_);
    ring_ = nullptr;
    ringHead_ = ringTail_ = ringCount_ = 0;
  }
  free(rxBuf_);
  rxBuf_ = nullptr;

  {
    Lock l(wsLock_);
    for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
      wsClients_[i] = WsClient{};
    }
  }
  {
    // Every session dies with the AP. A token minted before a disable must not
    // work after the next enable — the operator's mental model is "I turned it
    // off", and a surviving session would quietly contradict it.
    Lock l(authLock_);
    revokeAllLocked();
    // The grace clock goes with them. A window left armed across a disable
    // would fire on the next enable's first tick — revoking a session that
    // belongs to a different AP session entirely and rotating a PIN that was
    // just minted — and a stale `associated` flag would arm a fresh one
    // against a station that disassociated because we took the radio down.
    ApGrace::reset(grace_);
    // The secrets are not needed while the AP is down, so they do not sit in
    // RAM waiting for a memory-disclosure bug. The PSK comes back from NVS on
    // the next enable(), and so does pskSet_; the PIN does not come back at
    // all — a NEW one is minted, because it is RAM-only now.
    memset(psk_, 0, sizeof(psk_));
    memset(pin_, 0, sizeof(pin_));
    pskSet_ = false;
    // And out of the LCD's copy too. httpTick() stops being called the moment
    // the module is disabled, so without this the PIN would stay on the panel
    // indefinitely after the AP it belongs to had gone off the air.
    Pairing::withdraw();
  }
  teardownPending_.store(false, std::memory_order_release);
  stopping_.store(false, std::memory_order_release);
}

bool httpDisable(const char **errMsg) {
  (void)errMsg;

  // Refuse new registry work from the HTTP task, then find out whether any is
  // already in flight. See the shutdown note at the top of this file: this
  // store and the load below are the deadlock check, not bookkeeping.
  stopping_.store(true, std::memory_order_seq_cst);
  if (inRegistry_.load(std::memory_order_seq_cst) == 0) {
    teardownNow();
    return true;
  }

  // A handler is inside — or committed to entering — a registry call, and this
  // function is running WITH the registry lock held. httpd_stop() here would
  // join a task that cannot finish. Hand the teardown to the scheduler task
  // instead; it runs outside the lock, within one poll interval.
  teardownPending_.store(true, std::memory_order_release);
  return true;
}

// Body of the scheduler task exported at the bottom of this file.
void deferredTeardownPoll() {
  if (!teardownPending_.load(std::memory_order_acquire)) {
    return;
  }
  teardownNow();
}

void httpTick() {
  uint32_t now = millis();

  if (rebootPending_.load(std::memory_order_acquire) && (int32_t)(now - rebootAtMs_) >= 0) {
    // The response went out on the HTTP task some hundreds of ms ago. Restart
    // from HERE (the loop task) rather than from the handler, so the socket had
    // a chance to flush and close first.
    Serial.flush();
    esp_restart();
  }

  // ---- the session follows the Wi-Fi association (apgrace.h) ------------
  //
  // POLLED, not event-driven, and that is a decision rather than laziness. A
  // Wi-Fi event handler runs on the Wi-Fi task, which would make it a THIRD
  // task mutating sessions_ and pin_ — the two things authLock_ exists to
  // protect and the two things every comment in this file reasons about as
  // "loop task or HTTP task". This tick already runs every TICK_MS (250 ms),
  // so the cost is one esp_wifi call per tick and the worst-case lateness of
  // the 90-second window is 250 ms.
  //
  // OUTSIDE authLock_ ON PURPOSE: softAPgetStationNum() goes into esp_wifi and
  // can block on the Wi-Fi task, and nothing in this file holds a lock across
  // a call it does not control. fillApStatus() reads it the same way.
  uint8_t stations = apUp_ ? (uint8_t)WiFi.softAPgetStationNum() : 0;

  uint32_t endedId = 0;
  bool graceExpired = false;
  bool rotated = false;

  // SCOPED, so the bus event at the bottom is emitted with authLock_ RELEASED.
  // The sink takes ringLock_, and no path in this file takes authLock_ while
  // holding ringLock_; keeping it that way means the two never have to have a
  // documented order at all.
  {
    Lock l(authLock_);

    // Expired sessions are reaped opportunistically here as well as on lookup,
    // so a token that has aged out stops counting against MAX_SESSIONS even if
    // nobody ever presents it again. FIRST, because everything below is a
    // function of whether a session is live and a session that aged out three
    // minutes ago must not keep a grace clock armed.
    reapLocked(now);

    // Counted inline rather than via liveSessionCount(): authLock_ is a PLAIN
    // mutex (xSemaphoreCreateMutex, not recursive) and is already held here, so
    // calling that function would deadlock the loop task on its first tick.
    uint8_t live = 0;
    for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
      if (sessions_[i].used) {
        live++;
      }
    }

    if (!apUp_) {
      // The radio is down (a failed enable, or a teardown in flight). No
      // association means no association-derived session lifetime; anything
      // armed is meaningless and is dropped rather than allowed to fire later.
      ApGrace::reset(grace_);
    } else if (ApGrace::update(grace_, stations, live > 0, now) == ApGrace::EXPIRED) {
      // ---- MINT TRIGGER 3 OF 4: SESSION END -------------------------------
      //
      // 90 seconds with a live session and nobody associated. The phone is not
      // coming back, so the token it holds must stop working and the device
      // must become pairable again — which means BOTH halves: revoke, and
      // mint. Revoking alone would put the PIN that the departed client
      // already used back on the LCD, and that PIN is spent by definition
      // (trigger 2 consumed it at pairing).
      //
      // `live > 0` is the arming condition, not just a filter: a station that
      // associates and leaves WITHOUT pairing arms nothing at all. See the
      // long note at ApGrace::update() for why — the short version is that
      // rotating in that case defends against nobody (the PIN is on the LCD in
      // plain sight anyway) and breaks the flow of someone mid-way through
      // reading or scanning it.
      //
      // The id is captured BEFORE the revoke so the event can name what ended;
      // with MAX_SESSIONS == 1 there is at most one.
      //
      // The limiter is NOT touched here. A session ending is not evidence
      // about the person who was grinding PINs, and the lockout is theirs to
      // serve.
      for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions_[i].used) {
          endedId = sessions_[i].id;
          break;
        }
      }
      revokeAllLocked();
      live = 0;
      char rerr[128];
      rotated = mintPinLocked(rerr, sizeof(rerr));
      graceExpired = true;
    }

    // ---- the pairing PIN's out-of-band channel (pairing.h) ----------------
    //
    // Evaluated here, every TICK_MS, because both inputs move on their own: a
    // session can expire in reapLocked() above, and the last one being revoked
    // has to put the PIN back on the LCD without anyone asking.
    //
    // This is also the only place pin_ leaves this file, and it goes to a
    // buffer with no JSON representation and no transport — never into
    // status(), which is wire-visible. publish() is itself a no-op unless
    // `display` has subscribed, so with the LCD off the PIN never leaves this
    // file at all.
    //
    // shouldShow() is what keeps a PIN minted at pairing (trigger 2) off the
    // screen: `live` is 1 at that moment, so the branch below withdraws rather
    // than publishing, and the fresh PIN stays in RAM until the session ends.
    if (Pairing::shouldShow(apUp_, live)) {
      Pairing::publish(pin_);
    } else {
      Pairing::withdraw();
    }
  }

  // The revoke is visible on the console and to every WebSocket client, in the
  // same shape as every other auth outcome (fillAuthEvent). It carries what
  // ENDED and whether the PIN rotated — never the PIN, and never the token that
  // just stopped working. Emitted outside the lock; see the scope note above.
  if (graceExpired) {
    AuthEvt ev{"session_ended", 0, 0, endedId, false, rotated};
    Bus::emit("http.auth", fillAuthEvent, &ev);
  }
}

// ===========================================================================
// Actions
// ===========================================================================

// requirePhysical() USED TO LIVE HERE and is gone (backlog S6). `psk` and `pin`
// are declared AUTH_PHYSICAL in modauth.h and Registry::dispatch() refuses
// below it, with the same message every other refusal on this device carries.
//
// The point of the whole exercise is unchanged and is why those two rows are
// PHYSICAL: the AP's own passphrase and the pairing PIN are readable ONLY by
// someone holding the cable, so a stolen session token cannot be escalated into
// permanent access to the radio. `status` and `sessions`, which had their own
// hand-rolled AUTH_TOKEN checks, are declared TOKEN for the same reason they
// always were.

void fillApStatus(JsonObject d) {
  d["transport"] = "http";
  d["ap_up"] = apUp_;
  d["ssid"] = (const char *)ssid_;
  d["channel"] = AP_CHANNEL;
  d["security"] = "wpa2-psk/ccmp";
  d["max_clients"] = AP_MAX_CLIENTS;
  if (apUp_) {
    IPAddress ip = WiFi.softAPIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3]);
    // Either form is safe here; both copy. The real rule in ArduinoJson 7.4.3,
    // read off Strings/Adapters/RamString.hpp rather than inferred:
    //   const char *      -> StringAdapter<TChar*>          RamString(s, strlen)  COPIES
    //   char[N]           -> StringAdapter<TChar[N]>        RamString(s, strlen)  COPIES
    //   const char[N]     -> StringAdapter<const char(&)[N]> RamString(p, N-1, true)
    //                                                       STORES A POINTER
    // Only the third stores by reference, on the assumption it is a literal. It
    // is the one that bites, and it bites through a const& parameter: `r.msg`
    // where r is a `const Foo &` makes msg a const char[N] and the document ends
    // up referencing a dead frame (this happened once already — see the note at
    // renderActionResult in console.cpp).
    // So a stack buffer assigned uncast, or cast to const char*, is fine.
    d["ip"] = ipStr;
    d["clients"] = (uint8_t)WiFi.softAPgetStationNum();
  }
  // Explicit, even though it is `clients > 0`, because with AP_MAX_CLIENTS == 1
  // this is no longer a count — it is the session's lifeline, and it is what
  // design/BRIEF.md 4.2 needs to tell "live" from "reconnecting". A client
  // rendering that state should not have to know that the AP is single-client
  // to read `clients` correctly.
  //
  // OUTSIDE the apUp_ block, unlike `clients` and `ip`. With the AP down there
  // is definitively no station associated, so the honest answer is false rather
  // than absent — and `grace_ms`/`grace_active` beside it are unconditional, so
  // a client parsing the pairing state would otherwise have to handle one of
  // the three being missing while the others are present.
  d["station_associated"] = apUp_ && WiFi.softAPgetStationNum() > 0;
  d["server_up"] = server_ != nullptr;
  // A PASSPHRASE was minted by this enable — a virgin NVS, or a stored one that
  // failed its validator. That is the one credential an operator has to go and
  // read over USB, so it is the one worth flagging. It is NOT "both secrets"
  // any more: the PIN is minted on every enable without exception, so a flag
  // for it would be permanently true and carry no information, and the LCD is
  // where the PIN is meant to be read from anyway. See the note beside
  // credentialsNew_ for the full reasoning.
  d["credentials_new"] = credentialsNew_;
  d["teardown_pending"] = teardownPending_.load(std::memory_order_acquire);
  d["in_registry"] = inRegistry_.load(std::memory_order_relaxed);
  if (rebootPending_.load(std::memory_order_acquire)) {
    d["reboot_pending"] = (const char *)rebootReason_;
  }
  d["sessions"] = liveSessionCount();
  d["max_sessions"] = MAX_SESSIONS;
  d["ws_clients"] = wsCount();
  d["max_ws_clients"] = MAX_WS_CLIENTS;
  d["max_line"] = (uint32_t)MAX_WS_RX;
  d["idle_timeout_s"] = (uint32_t)(SESSION_IDLE_MS / 1000u);
  d["lifetime_s"] = (uint32_t)(SESSION_ABSOLUTE_MS / 1000u);
  d["sessions_issued"] = sessionsIssued_;
  d["sessions_expired"] = sessionsExpired_;
  d["sessions_evicted"] = sessionsEvicted_;
  d["ws_rejected"] = wsRejected_;
  d["events_sent"] = eventsSent_;
  d["events_dropped"] = eventsDropped_;
  // The grace window (apgrace.h), which is the state design/BRIEF.md 4.2 calls
  // "reconnecting": the station has gone but its token is still good. The UI
  // needs both halves — that a clock is running, and how much of it is left —
  // to render an honest countdown instead of an indefinite spinner, and to know
  // when to stop retrying and send the user back to the LCD.
  //
  // grace_ms is reported unconditionally so a client can size its own UI before
  // the window ever opens; grace_ms_left only while one is actually armed, so
  // its absence means "not in that state" rather than "zero seconds left".
  d["grace_ms"] = (uint32_t)ApGrace::GRACE_MS;
  {
    Lock l(authLock_);
    // grace_ is written only by httpTick() on the loop task, under this same
    // lock. This function usually runs on the HTTP task, so the lock is what
    // makes the pair of reads below consistent with each other rather than
    // straddling a tick.
    bool armed = ApGrace::armed(grace_);
    d["grace_active"] = armed;
    if (armed) {
      d["grace_ms_left"] = ApGrace::remainingMs(grace_, millis());
    }
    d["pin_fails"] = pinLimit_.totalFails;
    d["pin_lockouts"] = pinLimit_.lockouts;
    d["pin_locked"] = pinLimit_.locked;
    d["pin_attempts_remaining"] = RateLimit::remaining(pinLimit_);
  }
  // A stored PIN from a pre-2026-08-24 build that could not be erased. Almost
  // always false; false is not the same as "never checked", so it is reported
  // rather than assumed.
  if (stalePinKey_) {
    d["nvs_stale_pin_key"] = true;
  }
  // NOTHING here is a secret: no PSK, no PIN — not its value, not its length,
  // not the fact that it just changed — no token, no client MACs. The PIN's
  // only route out of this file is Pairing::publish() to the LCD (pairing.h),
  // and status() is wire-visible to every authenticated client.
}

// ===========================================================================
// `psk set` — replacing the AP passphrase, live
// ===========================================================================
//
// WHY THIS RESTARTS THE AP RATHER THAN THE WHOLE MODULE, and why that is safe
// with the registry lock held:
//
//   Registry::dispatch() holds the registry lock across this call (registry.h
//   says so), and the dispatch runs on whichever task delivered the command.
//   The ONE operation in this file that must never happen under that lock is
//   httpd_stop(), because it JOINS the HTTP server task — which may itself be
//   blocked waiting for that same lock. That is the deadlock the Dekker
//   handshake at the top of this file exists to detect, and it is the reason
//   `disable` sometimes has to defer.
//
//   Nothing below calls httpd_stop(). esp_http_server keeps running throughout;
//   its listening socket is bound to 0.0.0.0:80 and survives the AP netif
//   flapping. What actually gets restarted is the Wi-Fi AP:
//     esp_wifi_deauth_sta(0)          - throw every station off
//     WiFi.softAPdisconnect(false)    - AP.clear(): blank the AP config. `false`
//                                       keeps the netif, IP and DHCP server up;
//                                       `true` would call AP.end() and take the
//                                       whole interface down under us.
//     startAp(new)                    - AP.create(): the new config
//   None of those three joins the HTTP server task or takes the registry lock.
//   WiFi.softAP() waits on ESP_NETIF_STARTED_BIT (AP.cpp:200), which is already
//   set here and is posted by the Wi-Fi event task — a task this file never
//   blocks. So the restart is SYNCHRONOUS and the response can report what it
//   actually did. The deferred path is not needed and is not used.
//
//   Client sockets belonging to the dropped WebSocket clients are closed with
//   httpd_sess_trigger_close(), which queues the close onto the server task
//   (non-blocking here — see the threading note) rather than performing it.
//
// AUTH_PHYSICAL is checked by the caller before any of this runs.

struct RestartReport {
  uint8_t sessionsRevoked;
  uint8_t wsDropped;
  uint8_t clientsDisassociated;
  bool restarted;     // the radio was actually taken down and back up
  bool apUp;          // the AP's REAL state when this returned, not its intent
  bool reverted;      // the old passphrase was put back on the radio
  bool nvsReverted;   // ...and in NVS too
  bool pinRotated;    // a pairing was ended, so the spent PIN was replaced
};

// Drops every WebSocket client and closes its socket. Returns how many.
// The table is cleared under wsLock_ and the closes are triggered AFTER the
// lock is released — wsLock_ is never held across anything that touches a
// socket (see the lock-order note at the top).
uint8_t dropAllWsClients() {
  int fds[MAX_WS_CLIENTS];
  uint8_t n = 0;
  {
    Lock l(wsLock_);
    for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
      if (wsClients_[i].used) {
        fds[n++] = wsClients_[i].fd;
        wsClients_[i] = WsClient{};
      }
    }
  }
  for (uint8_t i = 0; i < n && server_ != nullptr; i++) {
    httpd_sess_trigger_close(server_, fds[i]);
  }
  return n;
}

// Writes the passphrase and its source flag to NVS as one unit. Returns false
// if either half did not land — the caller must not report success in that
// case, because the value would revert at the next boot.
bool persistPsk(const char *value, bool ownerSet) {
  Preferences w;
  if (!w.begin(NVS_NAMESPACE, false)) {
    return false;
  }
  bool ok = w.putString(NVS_KEY_PSK, value) == strlen(value);
  ok = ok && w.putUChar(NVS_KEY_PSK_SRC, ownerSet ? 1 : 0) == sizeof(uint8_t);
  w.end();
  return ok;
}

// Caller must NOT hold authLock_, and must have validated `fresh` already.
bool setPassphrase(const char *fresh, RestartReport *rep, CmdError *err) {
  *rep = RestartReport{};
  rep->apUp = apUp_;

  char old[sizeof(psk_)];
  bool oldSet;
  bool sameKey;
  {
    Lock l(authLock_);
    memcpy(old, psk_, sizeof(old));
    oldSet = pskSet_;
    sameKey = CT::equalStr(psk_, fresh, AuthFmt::PSK_MAX + 1);
  }

  if (sameKey && oldSet) {
    // Already live and already marked owner-chosen. Tearing the AP down to
    // arrive at the state we are already in would cost the caller their
    // session and every other client's, for nothing.
    memset(old, 0, sizeof(old));
    return true;
  }

  // 1. PERSIST FIRST. The failure that actually happens is an NVS one (full,
  //    unavailable, worn out), and taking it before the radio is touched means
  //    the common failure changes precisely nothing — the AP keeps running on
  //    the old passphrase and the caller is still associated.
  if (!persistPsk(fresh, true)) {
    cmdErrorf(err, "ENVS", "NVS write failed; the passphrase and the AP are unchanged");
    memset(old, 0, sizeof(old));
    return false;
  }

  if (sameKey) {
    // The owner re-typed the passphrase that was already on the air — only the
    // "generated"/"set" flag moved. The key has not changed, so nothing issued
    // under it is stale and nothing gets torn down.
    Lock l(authLock_);
    pskSet_ = true;
    memset(old, 0, sizeof(old));
    return true;
  }

  // 2. Everything minted under the old passphrase is cryptographically stale:
  //    a listener who had the old key saw the handshakes that produced these
  //    tokens. They go, and the caller is told how many.
  rep->clientsDisassociated = apUp_ ? WiFi.softAPgetStationNum() : 0;
  rep->sessionsRevoked = revokeAllSessions();
  rep->wsDropped = dropAllWsClients();
  // A re-key ends any pairing, and the general rule applies unchanged: a
  // revocation that actually revoked something rotates the PIN. See
  // rotateAfterRevocation() for the rule and for why the AP restart below —
  // including its failure and revert branches — is deliberately not an input to
  // it. The sessions are already gone at this point; whether the radio comes
  // back on the new key, the old key, or not at all cannot un-spend the PIN
  // that paired them, and leaving a spent PIN on the LCD is precisely the
  // outcome the rule exists to prevent.
  rep->pinRotated = rotateAfterRevocation(rep->sessionsRevoked, 0);

  if (!apUp_) {
    // Enabled but the radio is down (a failed enable, or a teardown in
    // flight). Nothing to restart; the new passphrase is stored and will be
    // used by the next enable().
    Lock l(authLock_);
    snprintf(psk_, sizeof(psk_), "%s", fresh);
    pskSet_ = true;
    memset(old, 0, sizeof(old));
    return true;
  }

  // 3. The restart itself.
  //
  // softAPdisconnect(false) is AP.clear() — it blanks the AP config and keeps
  // the netif, IP and DHCP server. Its result is deliberately NOT part of the
  // success test: the question that matters is whether the NEW config took,
  // which startAp() answers, and the deauth above has already thrown every
  // station off regardless. Treating a failed clear() as fatal would let a
  // framework quirk block the whole action for no gain.
  (void)esp_wifi_deauth_sta(0);  // aid 0 == every associated station
  (void)WiFi.softAPdisconnect(false);
  bool up = startAp(fresh);
  if (up) {
    apUp_ = true;
    rep->restarted = true;
    rep->apUp = true;
    Lock l(authLock_);
    snprintf(psk_, sizeof(psk_), "%s", fresh);
    pskSet_ = true;
    memset(old, 0, sizeof(old));
    return true;
  }

  // 4. The radio refused. Put back exactly what was there — on the air AND in
  //    NVS — so the module cannot end up persisted-new / running-old, or
  //    claiming an AP that is not there. apUp_ is set from what actually
  //    happened, so fillApStatus() tells the truth either way.
  bool back = startAp(old);
  apUp_ = back;
  rep->apUp = back;
  rep->reverted = back;
  rep->nvsReverted = persistPsk(old, oldSet);
  rep->restarted = true;  // the AP did go down, whatever came back
  if (back && rep->nvsReverted) {
    cmdErrorf(err, "EAPRESTART", "the AP would not restart; the old passphrase is back on the air and in NVS");
  } else if (back) {
    cmdErrorf(err, "EAPRESTART", "the AP would not restart; old passphrase is live but NVS still holds the new one");
  } else {
    cmdErrorf(err, "EAPRESTART", "the AP would not restart and would not come back; it is DOWN — disable/enable http");
  }
  memset(old, 0, sizeof(old));
  return false;
}

// Turns a rejected passphrase into a message that names the actual length and
// the limit. "invalid" on its own is what makes someone try the same wrong
// thing twice.
void passphraseRejection(AuthFmt::PassphraseCheck c, size_t len, size_t bad, CmdError *err) {
  switch (c) {
    case AuthFmt::PASSPHRASE_SHORT:
      cmdErrorf(err, "EARGS", "p.set is %u character(s); a WPA2 passphrase is %u..%u", (unsigned)len,
                (unsigned)AuthFmt::PSK_MIN, (unsigned)AuthFmt::PSK_MAX);
      break;
    case AuthFmt::PASSPHRASE_LONG:
      if (len == AuthFmt::PSK_MAX + 1) {
        // 64 characters is a raw 256-bit PMK in hex, which goes to a different
        // WPA2 code path entirely. Rejected rather than half-supported.
        cmdErrorf(err, "EARGS", "p.set is 64 characters: that is a raw hex PSK, not a passphrase; use %u..%u",
                  (unsigned)AuthFmt::PSK_MIN, (unsigned)AuthFmt::PSK_MAX);
      } else if (len >= AuthFmt::PSK_SCAN_CAP) {
        cmdErrorf(err, "EARGS", "p.set is %u+ characters; a WPA2 passphrase is %u..%u", (unsigned)len,
                  (unsigned)AuthFmt::PSK_MIN, (unsigned)AuthFmt::PSK_MAX);
      } else {
        cmdErrorf(err, "EARGS", "p.set is %u characters; a WPA2 passphrase is %u..%u", (unsigned)len,
                  (unsigned)AuthFmt::PSK_MIN, (unsigned)AuthFmt::PSK_MAX);
      }
      break;
    case AuthFmt::PASSPHRASE_BAD_CHAR:
      // The INDEX, never the byte: echoing it back would put a fragment of the
      // rejected string into a response and, if it is a near-miss of the real
      // passphrase, into whatever logs that response.
      cmdErrorf(err, "EARGS", "p.set byte %u is not printable ASCII; use 0x20..0x7e only", (unsigned)bad);
      break;
    default:
      cmdErrorf(err, "EARGS", "p.set must be a string of %u..%u printable ASCII characters",
                (unsigned)AuthFmt::PSK_MIN, (unsigned)AuthFmt::PSK_MAX);
      break;
  }
}

DispatchResult actPsk(JsonObjectConst p, JsonObject d, CmdError *err) {
  JsonVariantConst setv = p["set"];
  if (setv.isNull()) {
    Lock l(authLock_);
    d["ssid"] = (const char *)ssid_;
    d["psk"] = (const char *)psk_;
    // Which of the two this is matters: a generated passphrase is 64.4 bits and
    // unrelated to anything public, an owner-chosen one is whatever the owner
    // decided it should be. See the note beside psk_.
    d["source"] = pskSet_ ? "set" : "generated";
    d["len"] = (uint32_t)strlen(psk_);
    d["security"] = "wpa2-psk/ccmp";
    return DISPATCH_OK;
  }

  if (!setv.is<const char *>()) {
    cmdErrorf(err, "EARGS", "p.set must be a string of %u..%u printable ASCII characters",
              (unsigned)AuthFmt::PSK_MIN, (unsigned)AuthFmt::PSK_MAX);
    return DISPATCH_FAIL;
  }
  const char *fresh = setv.as<const char *>();
  size_t len = 0, bad = 0;
  AuthFmt::PassphraseCheck c = AuthFmt::checkPassphrase(fresh, &len, &bad);
  if (c != AuthFmt::PASSPHRASE_OK) {
    d["set"] = false;
    d["len"] = (uint32_t)len;
    d["min"] = (uint32_t)AuthFmt::PSK_MIN;
    d["max"] = (uint32_t)AuthFmt::PSK_MAX;
    passphraseRejection(c, len, bad, err);
    return DISPATCH_FAIL;
  }

  RestartReport rep;
  bool ok = setPassphrase(fresh, &rep, err);

  // THE PASSPHRASE ITSELF IS NEVER ECHOED. The caller just sent it, so a copy
  // in the response body buys nothing and widens where it can leak — into a
  // console scrollback, a WebSocket frame, whatever logged the reply. Only its
  // length goes back, which is what a UI needs to confirm the round trip.
  d["set"] = ok;
  d["len"] = (uint32_t)len;
  d["ssid"] = (const char *)ssid_;
  d["restarted"] = rep.restarted;
  d["ap_up"] = rep.apUp;
  d["sessions_revoked"] = rep.sessionsRevoked;
  d["ws_clients_dropped"] = rep.wsDropped;
  d["clients_disassociated"] = rep.clientsDisassociated;
  // Reported for the same reason the count of revoked sessions is: the operator
  // on the cable has just invalidated a pairing, and needs to know the device is
  // showing a different PIN now. The fact, never the value — `pin` is a separate
  // AUTH_PHYSICAL action.
  d["pin_rotated"] = rep.pinRotated;
  {
    Lock l(authLock_);
    d["source"] = pskSet_ ? "set" : "generated";
  }
  if (!ok) {
    d["reverted"] = rep.reverted;
    d["nvs_reverted"] = rep.nvsReverted;
  }
  return ok ? DISPATCH_OK : DISPATCH_FAIL;
}

DispatchResult actSessions(JsonObjectConst p, JsonObject d, CmdError *err) {
  JsonVariantConst rev = p["revoke"];
  uint32_t now = millis();

  if (!rev.isNull()) {
    if (rev.is<const char *>() && strcmp(rev.as<const char *>(), "all") == 0) {
      uint8_t n = revokeAllSessions();
      d["revoked"] = n;
      // 0 for the ended id: "all" may in principle have ended more than one,
      // and naming an arbitrary member of a set is worse than naming none of
      // it. n == 0 revoked nothing and therefore rotates nothing.
      d["pin_rotated"] = rotateAfterRevocation(n, 0);
      d["sessions"] = liveSessionCount();
      return DISPATCH_OK;
    }
    if (!rev.is<uint32_t>()) {
      cmdErrorf(err, "EARGS", "p.revoke must be a session id (number) or the string \"all\"");
      return DISPATCH_FAIL;
    }
    uint32_t id = rev.as<uint32_t>();
    if (!revokeSession(id)) {
      cmdErrorf(err, "ENOSESSION", "no live session with id %u", (unsigned)id);
      d["sessions"] = liveSessionCount();
      return DISPATCH_FAIL;
    }
    d["revoked"] = id;
    // revokeSession() returned true to get here, so exactly one ended.
    d["pin_rotated"] = rotateAfterRevocation(1, id);
    d["sessions"] = liveSessionCount();
    return DISPATCH_OK;
  }

  JsonArray arr = d["sessions"].to<JsonArray>();
  {
    Lock l(authLock_);
    reapLocked(now);
    for (uint8_t i = 0; i < MAX_SESSIONS; i++) {
      if (!sessions_[i].used) {
        continue;
      }
      JsonObject o = arr.add<JsonObject>();
      // The id, never the token. A session listing that echoed tokens would
      // turn one authenticated client into all of them.
      o["id"] = sessions_[i].id;
      o["age_ms"] = now - sessions_[i].createdMs;
      o["idle_ms"] = now - sessions_[i].lastSeenMs;
      o["expires_in_ms"] = SESSION_IDLE_MS - (now - sessions_[i].lastSeenMs);
    }
  }
  d["max_sessions"] = MAX_SESSIONS;
  return DISPATCH_OK;
}

DispatchResult httpDispatch(const CmdContext &ctx, const char *act, JsonObjectConst p, JsonObject d, CmdError *err) {
  (void)ctx;  // every action of this module is gated centrally (modauth.h)

  if (strcmp(act, "status") == 0) {
    fillApStatus(d);
    return DISPATCH_OK;
  }

  if (strcmp(act, "psk") == 0) {
    // AUTH_PHYSICAL, and the SAME level for reading and for writing —
    // deliberately so: a network client holding a valid session token must not
    // be able to read OR change the AP's passphrase. If it could, one stolen
    // token would become permanent access to the radio, and would lock the
    // owner out of their own device. Declared in modauth.h; a read and a write
    // are one action here, so the level covers both without a parameter gate.
    return actPsk(p, d, err);
  }

  if (strcmp(act, "pin") == 0) {
    if (p["regenerate"] | false) {
      char rerr[128];
      uint8_t killed = liveSessionCount();
      if (!regeneratePin(rerr, sizeof(rerr))) {
        cmdErrorf(err, "ERNG", "%s", rerr);
        return DISPATCH_FAIL;
      }
      d["regenerated"] = true;
      d["sessions_revoked"] = killed;
    }
    Lock l(authLock_);
    d["pin"] = (const char *)pin_;
    // Whether this same value is ALSO on the 160x80 ST7735 right now. It was a
    // hardcoded `false` with a note saying "when W5 lands"; W5 has landed, and
    // a field that always says false is worse than no field — an operator on
    // the cable would conclude the out-of-band channel is not working. The
    // out-of-band channel is the security asset ARCHITECTURE.md section 4
    // counts on, and when this reads true there was no need to come over USB
    // for the PIN at all.
    //
    // Pairing::visible() reports the LCD's copy, not the policy: it is false
    // when `display` is disabled (nothing ever subscribed, so publish() stored
    // nothing) and false when the device is already paired.
    //
    // NOT a leak of the PIN through a second route: this action is
    // AUTH_PHYSICAL, which has just handed the caller the PIN itself on the
    // line above. It must never be copied into status(), which is not.
    d["lcd"] = Pairing::visible();
    return DISPATCH_OK;
  }

  if (strcmp(act, "sessions") == 0) {
    return actSessions(p, d, err);
  }

  cmdErrorf(err, "EUNKNOWN", "unknown action for module 'http': \"%.16s\" (status/psk/pin/sessions)", act);
  return DISPATCH_FAIL;
}

void httpStatus(JsonObject d) { fillApStatus(d); }

// Static .rodata — this is what lets the embedded page render the module's
// controls without naming it (webui.h).
const ModuleParam PSK_PARAMS[] = {
    ModParam::str("set", false,
                  "omit to READ the current passphrase. 8..63 printable ASCII replaces it, restarts the AP and "
                  "drops every session, client and WebSocket. Ending a session rotates the pairing PIN, so the "
                  "LCD will show a new one."),
};

const ModuleParam PIN_PARAMS[] = {
    ModParam::flag("regenerate", false, "issue a NEW pairing PIN and revoke every session. Omit to just read the current one."),
};

const ModuleParam SESSIONS_PARAMS[] = {
    // A string, because that is the only one of the two accepted forms a text
    // field can produce. actSessions takes a session id as a JSON NUMBER or
    // the literal string "all"; a quoted number is rejected with EARGS, so the
    // numeric form has to go through the raw-JSON escape hatch. Stated here
    // rather than papered over — the fix, if stuart wants it, is one
    // strtoul() in actSessions, and that is a behaviour change, not a
    // descriptor change.
    ModParam::str("revoke", false,
                  "\"all\" revokes every session. Omit to just list them. A single id must be sent as a JSON "
                  "NUMBER (p:{\"revoke\":7}) — use the raw JSON box for that."),
};

constexpr ModuleAction HTTP_ACTIONS[] = {
    {"status", "AP and server state: ssid, ip, clients, sessions, WebSocket clients, event counters", nullptr, 0,
     ModAuth::requiredFor("http", "status")},
    {"psk",
     "the AP's WPA2 passphrase; set:\"...\" replaces it (8..63 printable ASCII) and restarts the AP, dropping "
     "every session and client and rotating the pairing PIN. USB console only (auth >= physical), both ways",
     MOD_PARAMS(PSK_PARAMS), ModAuth::requiredFor("http", "psk")},
    {"pin", "the pairing PIN. USB console only; regenerate:true issues a new one and revokes every session",
     MOD_PARAMS(PIN_PARAMS), ModAuth::requiredFor("http", "pin")},
    {"sessions", "list live sessions (ids only, never tokens), or revoke one / all of them",
     MOD_PARAMS(SESSIONS_PARAMS), ModAuth::requiredFor("http", "sessions")},
};
static_assert(ModAuth::allGated(HTTP_ACTIONS, sizeof(HTTP_ACTIONS) / sizeof(HTTP_ACTIONS[0])),
              "http: an action has no declared auth level");
static_assert(ModAuth::isModuleListed("http"), "http has no row in ModAuth::MODULES");
// The two secrets in the image. Asserted next to the table as well as in
// modauth.h, because this is the file a reader looking for them will open.
static_assert(ModAuth::requiredFor("http", "psk") == ModAuth::PHYSICAL, "http.psk must stay AUTH_PHYSICAL");
static_assert(ModAuth::requiredFor("http", "pin") == ModAuth::PHYSICAL, "http.pin must stay AUTH_PHYSICAL");

const ModuleDescriptor HTTP_MODULE = {
    .id = "http",
    .name = "Wi-Fi AP + HTTP",
    .category = "transport",
    // SHARED: a passive `wifiscan` can coexist with an AP on this silicon. Only
    // monitor mode (RES_WIFI EXCLUSIVE) evicts this module, by arbitration,
    // with neither module naming the other. See claims.h.
    .claims = Claims::claim(Claims::RES_WIFI, Claims::CLAIM_SHARED),
    // NEVER default-enabled: a device that broadcasts an AP out of the box is a
    // device that broadcasts an AP in someone's pocket.
    .defaultEnabled = false,
    // Nothing binds at boot. Wi-Fi and esp_http_server are runtime peripherals
    // and enable()/disable() really do start and stop them.
    .bootTimeBinding = false,
    .essential = false,
    .minAuth = ModAuth::moduleMinimum("http"),
    .enable = httpEnable,
    .disable = httpDisable,
    .dispatch = httpDispatch,
    .status = httpStatus,
    .actions = HTTP_ACTIONS,
    .actionCount = (uint8_t)(sizeof(HTTP_ACTIONS) / sizeof(HTTP_ACTIONS[0])),
    .tick = httpTick,
    .tickIntervalMs = TICK_MS,
};

}  // namespace

const ModuleDescriptor *httpModuleDescriptor() { return &HTTP_MODULE; }

void httpTransportPoll() { deferredTeardownPoll(); }
