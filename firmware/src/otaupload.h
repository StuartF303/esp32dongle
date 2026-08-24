// usbdongle W0 — the OTA DELIVERY path (backlog S5).
//
// S4 built the half that decides whether an image has proved itself
// (otahealth.{h,cpp} + otadecide.h). This is the half that gets an image into
// the inactive slot in the first place: esp_ota_begin / esp_ota_write /
// esp_ota_end, streamed, bounded, and with no staging copy anywhere.
//
// ---- WHY NOTHING IS STAGED IN LittleFS -----------------------------------
//
// The obvious-looking design — receive to /fs, verify, then copy into the slot
// — is worse in every dimension that matters here. It needs 1.25 MB of the
// 7.75 MB partition, writes every byte to NOR flash TWICE (halving the erase
// budget of a partition that also holds the web assets), and adds a whole
// second failure mode between "received" and "installed". The inactive app
// slot IS the safe place for an unverified image: it is 4 MB of flash nothing
// boots from, and esp_ota_end() runs a full image validation before anything
// can select it. So the image goes straight there.
//
// ---- WHAT THIS DOES NOT DO -----------------------------------------------
//
// IT NEVER REBOOTS. Writing an image, selecting it and restarting into it are
// three decisions, exactly as `ota boot` already separates the last two. On
// success this reports which slot it wrote and whether that slot is now the
// one otadata will boot; `reboot` is a separate, AUTH_PHYSICAL command.
//
// ---- TRANSPORT-AGNOSTIC ON PURPOSE ---------------------------------------
//
// run() pulls bytes through a callback rather than knowing about
// esp_http_server. mod_http.cpp supplies a reader over httpd_req_recv(); a BLE
// or CDC chunked uploader (neither exists) would supply its own, and the flash
// half — bounds, timeouts, hashing, abort discipline — would not be written
// twice. It is the same split otahealth.h makes against otadecide.h.

#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace OtaUpload {

// Transfer buffer. Small deliberately: it is malloc'd for the life of one
// upload and freed on every exit path, so an upload costs ~4 KB of heap plus
// whatever esp_ota_write's own bookkeeping needs, on a board with no PSRAM
// where Wi-Fi and the HTTP server are already up.
constexpr size_t BUF_SIZE = 4096;

// Smallest image we will even begin. An ESP32-S3 app image is an 8-byte
// esp_image_header_t + segment headers + an esp_app_desc_t at 0x20; anything
// under 4 KB cannot be one, and rejecting it here means esp_ota_begin never
// erases a slot for a body that was obviously a mistake.
constexpr uint32_t MIN_IMAGE_BYTES = 4096;

// No data at all for this long ends the upload with ETIMEOUT and an
// esp_ota_abort(). The reader reports each individual timeout; this is the
// budget across consecutive ones.
constexpr uint32_t STALL_TIMEOUT_MS = 15000;

// Whole-transfer deadline. 1.25 MB over a 2.4 GHz SoftAP is ~12 s; 300 s is
// generous enough that a slow link is not punished and short enough that a
// half-open socket cannot hold the OTA slot hostage forever.
constexpr uint32_t TOTAL_TIMEOUT_MS = 300000;

// What the reader callback managed. READ_TIMEOUT is NOT an error: it means the
// socket had nothing yet, and only the stall budget turns a run of them into
// one.
enum ReadStatus : uint8_t {
  READ_DATA = 0,   // *got bytes were produced (may be 0)
  READ_EOF,        // the peer closed / no more body is coming
  READ_TIMEOUT,    // nothing available right now
  READ_ERROR,      // socket error, or the transport is shutting down
  // THE AUTHORISATION FOR THIS UPLOAD HAS GONE. Distinct from READ_ERROR on
  // purpose: the socket is fine and the bytes are arriving, but the credential
  // that permitted the transfer stopped being valid part-way through. run()
  // turns this into EREVOKED / 401 rather than ECONN / 400, because "your
  // session was revoked" and "the link failed" call for completely different
  // things from the client, and a transport that could only say ECONN would
  // send a phone into a reconnect loop over an unpair.
  //
  // Transport-agnostic despite sounding HTTP-specific: any transport that can
  // authorise an upload can lose that authorisation mid-transfer.
  READ_UNAUTHORISED,
};

// Pulls up to `cap` bytes. Never blocks longer than the transport's own recv
// timeout, so the deadlines below stay meaningful.
typedef ReadStatus (*ReadFn)(void *ctx, uint8_t *buf, size_t cap, size_t *got);

// "Is this upload still authorised?" Called by run() at the point where the
// answer stops being recoverable — immediately before esp_ota_end(), i.e.
// before the image is finalised and before the optional boot-slot selection.
// Optional: a nullptr means the transport has no such notion and run() skips
// the check entirely.
//
// WHY BOTH THIS AND READ_UNAUTHORISED. They cover different windows and
// neither subsumes the other. The reader's check runs per chunk, so a
// revocation during the transfer is noticed within one 4 KB read and the slot
// is abandoned early. This one runs once, AFTER the last byte, and closes the
// window between "the final chunk was read" and "the image is selected" — a
// window that is short in wall-clock terms and is exactly where an operator
// typing `sessions revoke all` over USB to stop an upload they did not
// authorise would land.
typedef bool (*AuthFn)(void *ctx);

struct Params {
  // DECLARED UP FRONT, and required. Knowing the size means esp_ota_begin()
  // erases only the sectors the image needs instead of all 4 MB, and it turns
  // "the client hung up" from a silently truncated image into a specific error.
  uint32_t declaredLen = 0;
  bool haveSha = false;   // verify a SHA-256 over the received bytes before esp_ota_end()
  uint8_t sha[32] = {0};
  // Select the freshly written slot as the next boot partition. EXPLICIT in the
  // request, never implied: see the trade-off note above the upload handler in
  // mod_http.cpp for why this is available to an AUTH_TOKEN session at all.
  bool select = false;
  // Re-checked before the image is finalised and selected. nullptr == the
  // transport does not authorise uploads and there is nothing to re-check.
  AuthFn stillAuthorised = nullptr;
};

struct Report {
  bool ok = false;
  const char *code = nullptr;  // static wire code; nullptr on success
  char msg[176] = {0};
  uint16_t httpStatus = 500;   // suggested HTTP status for a transport that has one

  char target[17] = {0};       // the slot that was written ("app1")
  uint32_t targetOffset = 0;
  uint32_t targetSize = 0;
  uint32_t declared = 0;
  uint32_t written = 0;        // bytes actually handed to esp_ota_write()
  uint32_t elapsedMs = 0;

  bool shaChecked = false;
  bool shaOk = false;
  char sha256[65] = {0};       // hex of what was ACTUALLY received, always reported

  // Read back OUT OF THE WRITTEN PARTITION, not echoed from the request, so the
  // caller can confirm WHAT it just uploaded rather than what it meant to.
  char build[48] = {0};        // "<date> <time>" from esp_app_desc_t
  char idfVersion[32] = {0};
  char appVersion[33] = {0};
  char project[33] = {0};

  bool selected = false;         // esp_ota_set_boot_partition() succeeded on the new slot
  char bootPartition[17] = {0};  // what otadata names as the next boot, AFTER all of the above
  bool rebootRequired = false;   // bootPartition is not the running one
};

// Streams one image into the inactive OTA slot. Blocks for the whole transfer,
// so it must be called on a task that can afford that — the HTTP server task,
// never the loop task, and NEVER with the registry lock held.
//
// One upload at a time, image-wide: a second concurrent call is refused with
// EBUSY rather than being queued.
//
// On every failure path the handle is closed with esp_ota_abort() and otadata
// is left exactly as it was, so a partial upload cannot become a bootable
// selection.
bool run(const Params &p, ReadFn read, void *ctx, Report &rep);

// True while run() is executing. Cheap, lock-free, safe from any task.
bool busy();

// Current or last upload, for the `ota` built-in. Reads only.
void fillStatus(JsonObject d);

// Renders a finished Report into a response `d`. Separate from run() so a
// transport can put it wherever its envelope wants it.
void fillReport(JsonObject d, const Report &rep);

}  // namespace OtaUpload
