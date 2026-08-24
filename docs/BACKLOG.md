# Backlog

Deliberately deferred work, with the reasoning that deferred it. Items are here because they
were *noticed and decided against for now* — not because nobody thought of them.

Ordered by consequence within each section. See `ARCHITECTURE.md` for the plan and `CLAUDE.md`
for verified hardware facts.

---

## Security — do before this leaves your desk

**S3. The AP passphrase is a public secret, by choice.**
`pass-a9d8` is derivable from the broadcast SSID, and WPA2-PSK has no forward secrecy against a
holder of the key — a captured 4-way handshake exposes the PIN and session token too. Stuart
chose this knowingly for typeability (2026-08-16); recorded here so it is revisited on purpose,
not rediscovered. Mitigation if wanted: a typeable-but-private passphrase, or moving the PIN
exchange to something the air link cannot reveal.

**S9. A session token is now equivalent to arbitrary code execution.**
Stuart's decision, taken knowingly on 2026-08-17 with S5: `POST /api/ota` accepts an image AND
selects it at `AUTH_TOKEN`, so whoever holds a session can replace the firmware with anything that
passes `esp_ota_end()`'s validation and wait for the next restart. Everything else the auth model
protects is downstream of code that endpoint can replace. Combined with S3's public AP
passphrase, **the security boundary of this device is radio range.** Recorded here so it is
revisited on purpose, not rediscovered. The mitigations that remain: the PIN rate limiter,
session lifetimes, the AP never being default-enabled, and the fact that an upload never reboots.

**S10. A pre-2026-08-24 8-digit PIN is still readable in the `nvs` partition, and is accepted.**
The pairing PIN stopped being persisted on 2026-08-24 and `enable http` now calls
`Preferences::remove("pin")` on any key an older build left behind. That removes the key from the
API — nothing can read it and `nvs_get_str` returns NOT_FOUND — but NVS is log-structured, so the
call flips two bits in the page's entry-state bitmap and leaves the data entry untouched. Measured
on the device: one byte changed in 32 KB (`0x39: 0xa8 -> 0x80`), and the old PIN was still legible
at nvs offset `0x0d00` afterwards. It goes when NVS garbage-collects that page, on its own
schedule. **Not fixable without cost:** overwriting the value first would append a new entry and
still leave the original bytes, and the only true scrub is erasing the whole partition, which takes
the Wi-Fi PSK and every module's enable state with it. Accepted because the residue is a one-off
from a build that no longer exists, the value opens nothing, and the design property that matters —
no new PIN is ever written to flash — holds regardless. Recorded so the next person to dump the
partition finds an explanation instead of a live-looking credential.

**S12. The session token is a durable secret at a colliding origin (`http://192.168.4.1`).**
`webui.h` moved the token from `sessionStorage` to `localStorage` on 2026-08-24, and the reasoning
is sound and stands: `sessionStorage` does not survive a tab close or an iOS tab eviction, so a
phone that got evicted came back with no token while the device still held the session for up to
90 s — locked out of its own device, with no PIN on the LCD to re-pair with, which is the exact
lockout the single-session design exists to prevent. `localStorage` closes that.

**What it costs, which `sessionStorage` did not.** `localStorage` is keyed by ORIGIN and is
durable, and this origin is `http://192.168.4.1` — the ESP32 SoftAP default, and the default of a
long tail of travel routers, dashcams, cameras and drones. Any *other* device the phone later
joins that serves a page from that address gets same-origin read access to our token. It needs
hostile JS on that other device and the token has to still be live, so the probability is low, and
it is bounded by the 15-minute idle and 4-hour absolute caps. It is slightly compounded by the
page's `script-src 'unsafe-inline'`, which is what makes an injected script on such a page cheap
to write.

**Accepted, not fixed.** The lockout it prevents is certain and happens to the owner; the leak it
risks needs a second hostile device on the same default address. Recorded so it is revisited on
purpose. If it is ever revisited, the levers are: a non-default SoftAP address (a one-line
`softAPConfig()` and a change to every printed/QR'd URL, including the version-1 QR budget in
ARCHITECTURE.md); binding the token to something `localStorage` cannot carry across origins; or
clearing the key on a clean unpair, which helps only the tidy case.

**S11. A WebSocket may sit unauthenticated for as long as it likes, and there are only three.**
`mod_http.cpp` writes `wsClients_[].openedMs` in `wsAdd()` and **nothing ever reads it**. There is
therefore no handshake timeout: a socket that completes the HTTP upgrade and then never sends the
`auth` frame stays in the table indefinitely, `authed == false`, holding one of `MAX_WS_CLIENTS`
(3) slots. Three such sockets and the owner's own page cannot open one — `wsAdd()` returns false,
the client gets `EMAXWS` and is closed. It is a denial of the event channel only (REST and
`/api/cmd` are unaffected, and an unauthenticated socket can execute nothing — the gate above
`wsSessionStillLive()` sees to that), and it is reachable only by a party that already holds the
single AP slot, which under `AP_MAX_CLIENTS == 1` means they are already denying pairing to
everyone by squatting it. So it adds no exposure that S3's public passphrase does not already
carry. **The fix is small and deliberately not taken here:** reap `used && !authed &&
now - openedMs > N` from `httpTick()`, which needs a value for N — long enough for a phone on a
slow link to complete a handshake and an `auth` frame, short enough to matter — and that is a
number stuart should pick rather than one to invent mid-pass. Recorded because the field exists
and looks like the timeout is implemented.

---

## Correctness and robustness

**C1. `http.sessions.revoke` accepts a union no schema type can express.**
It takes a JSON *number* (session id) or the string `"all"` (`mod_http.cpp` `actSessions`).
Declared `P_STRING`, so a numeric id must go through the web UI's raw-JSON escape hatch. A
`strtoul` fallback in the handler is a one-line fix but is a behaviour change — deliberately left
for stuart to decide.

**C2. `storage.caps` requires `AUTH_TOKEN`.**
So an unauthenticated client cannot discover the limits it is required to negotiate against
(`max_chunk`, `max_path`). Defensible, slightly awkward; `caps` is the one action worth
considering at `AUTH_NONE`. Since S6 that would take TWO deliberate acts, by design: a new
explicit sentinel in `modauth.h` (0 means "undeclared" and fails closed to PHYSICAL, so there is
no way to spell AUTH_NONE by accident), and lowering `storage`'s own module bar, which currently
hides the module from an unauthenticated caller before any action is considered.

**C4. Deferred-teardown window in `disable http`.**
In the contended case the module reports disabled and `RES_WIFI` is released up to 20 ms before
the radio is actually down. Closing it properly needs an async disable in the registry contract.

**C5. `disable http` can block the loop task for up to 5 s.**
`httpd_stop()` joins, bounded by `recv_wait_timeout`, if a client is stuck mid-request. Bounded,
not unbounded, and `loopTask` has no WDT subscription by default.

**C6. `Activity` has no lock.**
One producer at a time by convention; worst case is one frame showing a mixed label. A mutex
would put FreeRTOS into a header the host tests compile, which is why it was skipped.

**C8. Storage is capped at 2 GiB per file.**
`off_t` is 32-bit signed here, so a legal 4 GiB−1 FAT32 file is not fully addressable.
Advertised honestly as `caps.max_file_offset`.

**C9. `Registry::list()` growth vs the WebSocket TX cap.**
The full descriptor set is now ~6–7 KB against `MAX_WS_TX` 16384. Fine today, unmeasured on
device, and every new module grows it. `/api/modules` over HTTP is chunk-streamed and unaffected.
S6/S7 added ~40 bytes per action (`min_auth` + `allowed`) and ~26 per module — about 1 KB on the
current 26-action surface, and it grows with the same multiplier every new module does.

**C10. `Protocol::MAX_LINE` costs 3 KB of RAM per line-framing transport.**
CDC and the WebSocket each carry a 4 KB line buffer; BLE will want a third. Worth revisiting if
RAM gets tight, which it will.

**C11. The grace window is bound to the station COUNT, not to the session holder.**
`ApGrace::update()` takes `uint8_t stations` (`apgrace.h`), so *any* association cancels a
departed holder's window. The header justifies that on the AP being single-client, but the
inference it actually needs is "the only party who can associate is the session holder", and the
PSK does not guarantee that — `psk set` exists precisely so the owner can share the passphrase, and
S3 notes it may be a public secret by choice. The scenario, which is ordinary rather than
adversarial: the owner pairs from phone A, phone A leaves, the 90 s window arms, and the owner's
laptop associates within it. The window cancels, phone A's now-unreachable session is preserved,
no PIN appears on the LCD, and the laptop cannot pair for the remaining `SESSION_IDLE_MS` — up to
15 minutes with the device looking paired and nothing able to reach it. Recovery is the USB cable
(`http sessions revoke:all`) or waiting it out. **Binding the window to the holder needs the
station MAC** — captured at pairing from `esp_wifi_ap_get_sta_list()` and compared per tick rather
than counted — which is a design decision about identifying a client by MAC, not a tweak, so it is
stuart's call rather than something to do quietly. Note the same list call would also make
`station_associated` mean "the holder is here" rather than "someone is here".

**C12. `httpd_sess_trigger_close()`'s return value is ignored at all seven call sites.**
`mod_http.cpp` calls it from `drainEvents()` (2 — the dead-session drop and the failed send),
`handleWs()` (4 — `EMAXWS`, the over-long frame, the rejected `auth` frame and the no-live-session
gate) and `dropAllWsClients()` (1), and every one discards the `esp_err_t`. (Counted, not
estimated: the pass-A review said five.) It queues the close onto the server
task by writing to esp_http_server's control socket, and that write can fail — a full control
socket returns an error rather than blocking, which is the same non-blocking property
`httpd_queue_work()` relies on and the same one the event ring is written around. When it fails
the close never happens and the fd leaks until the peer drops the connection or `lru_purge_enable`
reclaims it. **No capability is retained**: `wsRemove()` has already cleared the entry, so the
socket is out of `wsClients_`, receives no events, and any frame arriving on it fails the gate at
`wsSessionStillLive()` — what leaks is a file descriptor, not an authorisation. The bound is
`max_open_sockets` (6) and LRU purge, and the trigger is a control socket full enough to reject a
write, which needs sustained event pressure. Fixing it properly means deciding what to DO on
failure — retry from the tick, force `close(fd)` from the wrong task, or accept it — and the
middle option races the server task, so this is a design decision rather than a missing `if`.

---

## Feature work (planned, not started)

**F1. BLE GATT transport.** The remaining half of W2. Will want ~40 KB of heap against the
~145 KB currently free with Wi-Fi up — a genuine squeeze, not a formality. Also the first
transport that will hit S1.

**F2. `msc` module** — expose the SD card to the host PC as a drive. Claims USB shared and SD
exclusive, mutually exclusive with `storage` over the card. Note the TinyUSB endpoint budget:
CDC+HID+MSC uses 4 of 5 IN endpoints, leaving exactly one spare. No WebUSB or second CDC after
this.

**F3. `wifiscan` / `blescan` modules.** Claim `RES_WIFI` / `RES_BLE` shared; monitor mode takes
`RES_WIFI` exclusive and must be arbitrated against the `http` transport, which is the user's
only link.

**F4. Wi-Fi STA mode.** Currently SoftAP only. Needed for the dongle to reach a real network, and
for MQTT later.

**F6. Real web UI (W4), served from LittleFS.** The current page is bring-up quality in PROGMEM.
Now that action params are machine-readable, a proper UI can render itself from the descriptors.

**F7. `hid` macros stored on the SD card.** The obvious `hid` + `storage` integration and the
actual point of a macro pad. Note macros are layout-dependent — see the en_GB work.

**F8. Captive portal and mDNS.** So joining the AP lands you on the page without typing an IP.

---

## Tooling and hygiene

**T1. Adafruit GFX drags in `Wire`.** ~8.5 KB flash and 204 B RAM of I2C machinery on a board
with no I2C bus, because `Adafruit_SPITFT.h` includes `Adafruit_I2CDevice.h`. LovyanGFX or
TFT_eSPI would avoid it, at the cost of losing the exact vendor-matching panel tab.

**T2. Unpinned packages in `libdeps`.** `Adafruit_ST7735`'s `library.properties` declares `SD` and
`seesaw` for its *examples*; both land in `.pio/libdeps` unpinned and unused, against this
project's pin-everything rule. Vendoring the library into `lib/` is the only clean fix.

**T3. `firmware/README.md`'s layout table is stale.** Missing most modules added since W1.

**T4. No CI.** `pio run` for both envs plus `pio test -e native` (the host suite — count lives in `README.md`, deliberately not repeated here) is a natural gate,
and the native tests already cover the security-critical path sanitisation.
Evidence that this is not theoretical: `README.md`'s figure had drifted from 229 to 236 without
anyone noticing, because nothing checks it — it is only ever corrected when someone happens to run
the suite and read the total.

---

## Resolved, kept for the reasoning

- **F5 — LittleFS was mounted by nothing. Done 2026-08-17.**
  `src/fsmount.{h,cpp}` mounts it at boot as **platform infrastructure**, like NVS — not as a
  module, so nothing has to be enabled for the web assets to be readable and there is no enable
  ordering between it and `http`. It can never prevent boot, and it **never auto-formats**: an
  unformatted partition and a corrupt one both come back `ESP_FAIL` from
  `esp_vfs_littlefs_register()`, so answering that by erasing would mean the first boot after a
  bad power cut silently destroys the owner's assets with a successful mount as the only
  evidence. `storage format p:{volume:"fs",confirm:true}` at **AUTH_PHYSICAL** is the explicit
  remedy and the only thing in the image that erases the partition.

  `storage` reaches both filesystems through **one** surface with a volume prefix (`/sd/...`,
  `/fs/...`), parsed by `src/volpath.h` — dependency-free, host-tested (14 cases), and strictly
  IN FRONT of `PathSafe`, which is unchanged and still the only path checker in the image. The
  volume root is spelled `/sd`, never `/sd/`. Unknown volume is `EVOLUME`, unmounted is
  `ENOTMOUNTED` naming which one and why; `caps` reports both volumes with per-volume limits and
  free space. `enable storage` now succeeds on **either** volume — an absent card no longer takes
  LittleFS with it — while still claiming `RES_SD` shared, because claims are static in this
  registry (see the note above `storageEnable()`).

- **F5's sibling: S5 — no OTA delivery path. Done 2026-08-17.**
  `src/otaupload.{h,cpp}` + `POST /api/ota` in `mod_http.cpp`. Streams straight into the inactive
  slot; nothing is staged in LittleFS. Size declared up front and enforced against both
  `Content-Length` and the bytes received, optional SHA-256 verified before `esp_ota_end()`, 4 KB
  buffer, hard cap at the slot size, 15 s stall / 300 s total deadlines, `esp_ota_abort()` on
  every failure path and `otadata` untouched unless the write succeeded AND `?select=1` was
  asked for. Never reboots. Selection goes through the same `OtaHealth::setBootNow()` `ota boot`
  uses, which is what puts the new image into `ESP_OTA_IMG_NEW` and therefore actually arms S4's
  confirmation machinery. See ARCHITECTURE.md §4 "OTA delivery". The security trade-off it
  carries is S9 above.

- **S6 / S7 / S8 — the module side of the auth model. Done 2026-08-17.**
  The same fix as S1, one layer down. `firmware/src/modauth.h` is now the single `.rodata` policy
  table for every module and every action, dependency-free and host-tested (21 cases,
  `test/test_modauth`) because the native env excludes `src/` — a level living in a `mod_*.cpp`
  could never be asserted anywhere. `ModuleAction` and `ModuleDescriptor` carry a `minAuth`
  initialised from that table at compile time, and `Registry::dispatch()` enforces it through the
  pure `ModAuth::decide()` **before** the ENOMOD / EDISABLED / EREBOOT branches, so the module map
  and each module's enabled state are no longer readable below the bar. `Registry::list()` applies
  the same rule and additionally emits `min_auth` per module and `min_auth` + `allowed` per action,
  which is what lets the phone UI grey out what a session cannot use.

  **The levels:** everything `AUTH_TOKEN`; `storage.format`, `http.psk`, `http.pin` stay
  `AUTH_PHYSICAL`; nothing at `AUTH_NONE`, and no way to spell it — an undeclared level is 0,
  which `effective()` reads as PHYSICAL and `allGated()` turns into a build failure. Stuart's two
  standing decisions are untouched: arming `hid` is `enable` (a TOKEN built-in), and OTA upload +
  select stay TOKEN (S9).

  **Deleted, not moved:** `mod_hid.cpp`'s `requireInjectAuth`, `mod_storage.cpp`'s `requireAuth`
  and `requirePhysical`, `mod_http.cpp`'s `requirePhysical` and its two inline `status`/`sessions`
  checks, and `mod_display.cpp`'s backlight check. Every one of them was fully expressed by an
  action-level declaration; nothing was kept. Two behaviour changes fell out: `hid.release`, which
  the module allowed at any level as a "panic stop", is TOKEN like everything else, and an action
  name absent from a module's descriptor table now answers EAUTH rather than EUNKNOWN to a caller
  below PHYSICAL (fail-closed, and it stops action enumeration).

  S8 is closed by a `static_assert` in console.cpp tying `CmdAuth::requiredFor("led")` to
  `ModAuth::requiredFor("led", "set")`: the built-in is a pure alias, so changing either alone now
  fails the build. C7 (`display.screen` / `refresh` ungated) is closed by the same table.

- **S1 — built-in commands had no auth check.** Done 2026-08-17. Gated centrally in
  `Console::execute` from a single `.rodata` policy table, enforced BEFORE `findCommand()` so an
  unauthenticated caller cannot tell `EAUTH` from `EUNKNOWN` and enumerate the surface. Unlisted
  commands fail closed at `PHYSICAL`. That is the property BLE relies on to dispatch honestly at
  `AUTH_NONE`.
- **S2 — `reboot` over the network.** Closed as a side effect of S1: `reboot` is `AUTH_PHYSICAL`
  and a network session can never reach that level, by design.
- **S4 — OTA rollback was never confirmed. Done 2026-08-17, on a corrected premise.**
  The item claimed `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` was not set. **It was**, and so was
  `CONFIG_APP_ROLLBACK_ENABLE` — lines 424 and 4392 of
  `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig`, and the prebuilt
  bootloader we flash carries `set_actual_ota_seq`, which IDF compiles only under that option.
  So the bug was the *worse* one: the bootloader was armed, nothing called
  `esp_ota_mark_app_valid_cancel_rollback()`, and the first OTA would have appeared to work and
  then silently reverted at the next restart. USB flashing hid it, because it leaves `otadata`
  erased (`ota_state UNDEFINED`, as `info` reports).

  **Done:** `src/otadecide.h` (pure decision table, 16 host tests) + `src/otahealth.{h,cpp}`
  (`ota.health` task at 250 ms). Five criteria — scheduler ticking, registry restore not fatal,
  essential `cdc` up, a request answered on any transport *or* a 20 s grace, and 30 s uptime —
  then mark-valid; a 180 s window, after which it marks invalid and reboots rather than leaving
  the image pending. Wi-Fi is deliberately **not** a criterion: `http` is `defaultEnabled=false`,
  so requiring the AP would roll back good builds on the factory default. Bus events
  `ota.pending` / `ota.confirmed` / `ota.rollback` / `ota.confirm_failed`; the LCD footer shows
  it through `activity.h` with no new region. New `ota` built-in reports the state and forces
  either transition at `AUTH_PHYSICAL`, refusing a rollback into a slot `otadata` does not bless.
  See ARCHITECTURE.md §4 "OTA rollback — armed by the bootloader, confirmed by the app".

  **Not done, and not in scope here:** the delivery path (S5). Also *untested on hardware* —
  reaching `PENDING_VERIFY` needs `otadata` moved by hand, since nothing in the image can do it.

- **C3 — armed-but-pending `hid` invisible.** Done 2026-08-17: hollow yellow badge, distinct from
  both solid-red live and no badge. This is the agreed mitigation for stuart's S1 choice that
  arming stays at `AUTH_TOKEN` — a session can arm the keyboard but not reboot to bind it, so the
  screen is what makes a pending arm visible rather than it landing silently at the next power-up.

- **LCD ownership vs the PIN.** An out-of-band channel a client can draw on is not out-of-band.
  Resolved 2026-08-17: the screen is entirely device-owned, no draw/text/image action exists, and
  that is what makes the PIN display trustworthy.
- **USB mode.** `hid`/`msc` need composite CDC+HID+MSC, which the fixed-function USB-Serial/JTAG
  peripheral cannot provide. Measured both, adopted TinyUSB, lost USB-JTAG — which was never
  reachable on this board anyway (GPIO 39/40 are the APA102, 41/42 are not broken out).
- **`defaultEnabled` on an existing device.** A newly added default-on module used to stay off
  forever. Fixed by persisting the known-module set, with an absent known-set treated as
  "everything is known" for one boot so the first upgrade cannot resurrect anything the owner
  disabled.
