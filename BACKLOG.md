# Backlog

Deliberately deferred work, with the reasoning that deferred it. Items are here because they
were *noticed and decided against for now* — not because nobody thought of them.

Ordered by consequence within each section. See `ARCHITECTURE.md` for the plan and `CLAUDE.md`
for verified hardware facts.

---

## Security — do before this leaves your desk

**S1. Built-in commands have no auth checks at all.**
`info`, `parts`, `mem`, `uptime`, `tasks`, `bootprobe`, `reboot` are dispatched without ever
consulting `ctx.authLevel`; only *modules* check. Nothing is exposed today because
`mod_http.cpp` returns 401 before dispatching, but that makes the HTTP transport the only thing
holding the line, and the design says modules decide their own auth. Any future transport that
dispatches at `AUTH_NONE` — BLE is next — instantly exposes `reboot` and full hardware detail to
an unauthenticated stranger. Gate the built-ins, then let transports dispatch honestly.

**S2. `reboot` is reachable at `AUTH_TOKEN` over the network.**
A phone with a valid session can restart the device. Probably fine for a personal tool; decide
deliberately rather than by omission. Related to S1.

**S3. The AP passphrase is a public secret, by choice.**
`pass-a9d8` is derivable from the broadcast SSID, and WPA2-PSK has no forward secrecy against a
holder of the key — a captured 4-way handshake exposes the PIN and session token too. Stuart
chose this knowingly for typeability (2026-08-16); recorded here so it is revisited on purpose,
not rediscovered. Mitigation if wanted: a typeable-but-private passphrase, or moving the PIN
exchange to something the air link cannot reveal.

**S4. OTA rollback is not actually enabled.**
The partition table has two OTA slots, but `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is not set and
nothing calls `esp_ota_mark_app_valid_cancel_rollback()`. `ARCHITECTURE.md` claims "a bad build
rolls back instead of bricking"; today it would not. Needs a real health check to gate the
mark-valid call — Wi-Fi and the console up, say. Until then a bad OTA is a USB recovery.

**S5. No OTA update path exists at all.**
Dual slots and 7.75 MB of LittleFS, and updates still require USB. This is the payoff the whole
16 MB repartition was for.

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

**C3. `hid` armed-but-pending-reboot is invisible on the LCD.**
The HID badge tracks *live* state. "Armed, reboots into a keyboard" arguably deserves a warning
glyph too — that is exactly the state worth noticing from across the room.

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

**F5. LittleFS is mounted by nothing.** 7.75 MB sitting unused. Needed for web assets, macro
storage, and OTA staging.

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
