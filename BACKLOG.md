# Backlog

Deliberately deferred work, with the reasoning that deferred it. Items are here because they
were *noticed and decided against for now* — not because nobody thought of them.

Ordered by consequence within each section. See `ARCHITECTURE.md` for the plan and `CLAUDE.md`
for verified hardware facts.

---

## Security — do before this leaves your desk

**S6. Module-side actions still have no auth check.**
S1 gated the built-ins; several module actions remain open by their own choice: `led.set`,
`led.auto`, `hid.status`, `storage.status`, `display.status`, `display.screen`, `display.refresh`.
None are exposed today (HTTP still 401s pre-dispatch), but they are the same class as S1 and will
matter the moment BLE dispatches at `AUTH_NONE`. `hid.status` in particular tells a stranger
whether the dongle is currently a keyboard.

**S7. `Registry::dispatch()` answers before the module's gate.**
`ENOMOD` / `EDISABLED` / `EREBOOT` are returned prior to the module's own auth check, so the
module map and each module's enabled state are readable at any auth level.

**S8. `led` alias and `led` module now disagree.**
The `led` built-in is `AUTH_TOKEN` after S1; `{"mod":"led","act":"set"}` is ungated. Same
capability, two different bars. Reconcile in whichever direction you prefer.

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
considering at `AUTH_NONE`.

**C4. Deferred-teardown window in `disable http`.**
In the contended case the module reports disabled and `RES_WIFI` is released up to 20 ms before
the radio is actually down. Closing it properly needs an async disable in the registry contract.

**C5. `disable http` can block the loop task for up to 5 s.**
`httpd_stop()` joins, bounded by `recv_wait_timeout`, if a client is stuck mid-request. Bounded,
not unbounded, and `loopTask` has no WDT subscription by default.

**C6. `Activity` has no lock.**
One producer at a time by convention; worst case is one frame showing a mixed label. A mutex
would put FreeRTOS into a header the host tests compile, which is why it was skipped.

**C7. `display`'s `screen` and `refresh` are ungated.**
Switching to `diag` hides the PIN. Not an attack (hiding a secret is not disclosing it), but it
is an unauthenticated state change.

**C8. Storage is capped at 2 GiB per file.**
`off_t` is 32-bit signed here, so a legal 4 GiB−1 FAT32 file is not fully addressable.
Advertised honestly as `caps.max_file_offset`.

**C9. `Registry::list()` growth vs the WebSocket TX cap.**
The full descriptor set is now ~6–7 KB against `MAX_WS_TX` 16384. Fine today, unmeasured on
device, and every new module grows it. `/api/modules` over HTTP is chunk-streamed and unaffected.

**C10. `Protocol::MAX_LINE` costs 3 KB of RAM per line-framing transport.**
CDC and the WebSocket each carry a 4 KB line buffer; BLE will want a third. Worth revisiting if
RAM gets tight, which it will.

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

**T4. No CI.** `pio run` for both envs plus `pio test -e native` (153 cases) is a natural gate,
and the native tests already cover the security-critical path sanitisation.

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
