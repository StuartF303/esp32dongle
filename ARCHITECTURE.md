# T-Dongle-S3 — architecture & feature plan

Status: **design, not yet implemented.** Decided 2026-08-15 with stuart.

Decisions taken:

- Must work from **both iOS and Android** phones.
- **All transports** are in scope — the core is transport-agnostic, transports are adapters.
- Tool scope: **USB HID / macro pad**, **wireless storage / file bridge**, **wireless recon /
  diagnostics**, plus a general platform for whatever comes next.
- **Repartitioning is authorised.** The factory image in `backup/factory_release/` is the
  fallback and must stay intact and restorable over USB.

---

## 1. The one hard constraint

**ESP32-S3 has BLE only.** Espressif removed Bluetooth Classic from the S3 — no SPP, no PAN,
no tethering profile. A phone browser therefore *cannot* fetch HTTP over Bluetooth. "Serve the
web UI over Bluetooth" is not achievable on this silicon, on any phone.

Consequences:

- **iOS has no Web Bluetooth** and Apple has shown no intent to ship it. So a browser-based UI
  reaching the dongle over BLE is Android-only, permanently.
- To hit both platforms with a browser, the UI must arrive over **Wi-Fi** (SoftAP or STA).
- BLE remains valuable for what Wi-Fi is bad at: always-on low-power advertising, presence,
  Wi-Fi provisioning, and instant single-shot actions without a 3–5 s association delay.

So: **BLE is a control channel, Wi-Fi is the UI channel.** They are complements, not rivals.

### Second constraint: 512 KB SRAM, no PSRAM

Wi-Fi + BLE + HTTP server + LVGL all hot at once is genuinely tight. Mitigations baked into
the design: NimBLE instead of Bluedroid (~40 KB saved), gzipped static assets served straight
from flash rather than assembled in RAM, transports individually disableable at runtime, and
LVGL only when the LCD module is enabled.

---

## 2. Shape: transport-agnostic core

```
                 ┌───────────────────────────────────────┐
   USB CDC ──────┤                                       │
   BLE GATT ─────┤   transport adapters  →  command bus  │
   HTTP/WS ──────┤                                       │
   (MQTT later) ─┤                                       │
                 └──────────────────┬────────────────────┘
                                    │
                          ┌─────────▼─────────┐
                          │  module registry  │  enable/disable, resource claims
                          └─────────┬─────────┘
                                    │
        ┌───────────┬───────────┬───┴───────┬───────────┬───────────┐
      hid         msc       storage      wifiscan    blescan     display
```

Every transport speaks the **same command protocol** and does nothing else. A module never
knows which transport a command arrived on, and a new transport costs one adapter file.

### Command protocol

Line-delimited JSON. Request/response are correlated by `id`; events are unsolicited.

```jsonc
// request
{"id": 7, "mod": "hid", "act": "type", "p": {"text": "hello"}}
// response
{"id": 7, "ok": true, "d": {}}
{"id": 7, "ok": false, "e": {"code": "EBUSY", "msg": "..."}, "d": {"partial": "..."}}
// queued: the work was accepted, completion arrives later as an event
{"id": 7, "ok": true, "accepted": true, "d": {"job": 3}}
// event
{"ev": "wifiscan.result", "d": {"ssid": "...", "rssi": -62}}
```

**`d` survives an error** (added 2026-08-16). An error response carries `e` *and* any
non-empty `d`; only an empty `d` is omitted. Failure and partial data are not alternatives:
a self-test that ran fine and found three broken cases, a directory listing with two
unreadable entries, a scan truncated by a timeout — all of them have to report the failure
*and* hand back what they got. The earlier rule (drop `d` on any error) forced every such
command to choose, and they all chose `ok:true` with the real outcome buried in a counter.

**Three dispatch outcomes, not two.** `DISPATCH_ACCEPTED` exists because the scheduler is
cooperative: `hid.type` of a long macro is ~8 ms per two HID reports and would hold the loop
task for tens of seconds if it ran inline. Accepted work returns a job handle in `d` and
reports completion as an event correlated by the request `id`.

Rationale for JSON over CBOR: debuggable by hand over the serial console, and the payloads are
small. If BLE throughput becomes a real problem the encoding is one layer to swap — the module
API doesn't change.

### Transport adapters

| Transport | Framing | Notes |
|---|---|---|
| **USB CDC** | newline-delimited | Free, always present, ideal for development. Composite with HID. |
| **HTTP + WebSocket** | WS text frames; REST mirror at `/api/*` | The phone web UI. Serves gzipped assets from LittleFS. |
| **BLE GATT** | Nordic-UART-style RX/TX characteristics, chunked to MTU | Provisioning + quick actions. Native app or Android Web Bluetooth. |
| **MQTT** *(later)* | topic per direction | Only meaningful in STA mode; deferred. |

### Module registry

Each module registers a descriptor rather than being wired in by hand:

```c
{
  .id              = "hid",
  .name            = "USB Keyboard",
  .category        = "input",
  .claims          = Claims::claim(RES_USB, CLAIM_SHARED),
  .defaultEnabled  = false,          // hid is opt-in, always
  .bootTimeBinding = true,           // its USB interface binds before setup()
  .essential       = false,
  .enable = ..., .disable = ..., .dispatch = ..., .status = ...,
  .actions = HID_ACTIONS, .actionCount = 3,   // static, .rodata
  .tick = hidTick, .tickIntervalMs = 10,      // registry registers & gates it
}
```

`GET /api/modules` returns the descriptor list, and the web UI renders itself from that — so a
new module needs **zero** front-end changes. That only works if the descriptor says what the
module *does*, so it carries an **action table** (`act` / `help` / `params`), and a computed
**`blocked_by`** array per module — the same `firstConflict()` walk `enable()` uses, so the
arbitration rule the UI displays cannot drift from the one the device enforces.

**`bootTimeBinding`** is the escape hatch for hardware whose real gate is boot, not runtime.
A TinyUSB interface is registered from a C++ static constructor and the descriptor set is
frozen by `USB.begin()` in `app_main`, before `setup()` runs — so `hid` cannot be started
later, by any means. Enabling such a module records the intent, persists it, and returns
`pendingRestart`; the module decides whether to bind by reading the same persisted set from
its own static constructor (`ModulePersist::wasEnabledAtBoot()`).

**Two levels of gating, both needed:**

- **Compile-time** (`menuconfig` / build flags) — what is in the binary at all. Flash and RAM
  are the budget.
- **Runtime** — what is active now, persisted in NVS, toggled from the phone.

Runtime claims are what make this more than cosmetic. Each module declares, per resource,
`NONE` / `SHARED` / `EXCLUSIVE`; a module may start iff every resource it wants exclusively is
unheld and every resource it wants shared is not held exclusively. The registry refuses the
impossible combination with a clear error naming every blocker, instead of hanging.

**Claim model, corrected 2026-08-16 against the framework rather than intuition:**

- **USB is `SHARED`, for everyone.** TinyUSB on this framework builds ONE composite device
  descriptor at boot: CDC, HID and MSC coexist, which is the entire reason `ARDUINO_USB_MODE=0`
  was adopted. Marking USB exclusive would be inventing a physical constraint that does not
  exist — and would make the serial console uncoexistable with both tools. Any hid/msc mutual
  exclusion is therefore a **policy** those modules enforce themselves, not a resource conflict.
- **`msc`'s real exclusivity is over SD**, which it hands to the host PC as a raw block device.
  That is what locks out `storage`.
- **The radio is two resources, `wifi` and `ble`, not one.** They behave identically while both
  are shared, but conflating them is wrong at exclusive: Wi-Fi monitor mode must lock out other
  Wi-Fi users and has no business evicting `blescan`. NVS persists module *ids*, never resource
  indices, so this split cost no migration.
- **The transports are modules too.** `cdc` claims USB shared and is `essential`: it is enabled
  at every boot regardless of NVS and cannot be disabled. Before that, a `force` enable could
  have reported `stopped: []` while taking away the only link to the device.

---

## 3. Partition plan (16 MB)

Current factory layout wastes 12 MB and has **no OTA slot**. Replacement:

| Offset | Size | Name | Purpose |
|---|---|---|---|
| `0x0` | 32 K | bootloader | |
| `0x8000` | 4 K | partition table | |
| `0x9000` | 32 K | nvs | config, module enable state, Wi-Fi creds, auth PIN/token |
| `0x11000` | 4 K | nvs_keys | reserved, `encrypted` — makes NVS encryption possible later |
| `0x12000` | 8 K | otadata | |
| `0x14000` | 48 K | *(pad)* | deliberate — aligns app0 to the next 64 K boundary |
| `0x20000` | 4 M | app0 (ota_0) | |
| `0x420000` | 4 M | app1 (ota_1) | OTA target + rollback |
| `0x820000` | 7.75 M | littlefs | web assets, macros, small config and log files |
| `0xfe0000` | 128 K | coredump | |

Sizing decided 2026-08-15 with stuart, after review. The three non-obvious calls:

- **`nvs` 20 K → 32 K.** 20 K was inherited from the factory table, where it held almost
  nothing. It now has to carry Wi-Fi STA config, ~6 module enable flags, an auth PIN and a
  rotating session token — and NVS needs spare pages for compaction and wear levelling, so a
  nearly-full NVS misbehaves rather than simply filling up.
- **`nvs_keys` reserved.** NVS encryption requires a dedicated 4 K partition. Reserving it now
  costs 4 K; retrofitting it later costs a full USB reflash. Encryption is *not* enabled yet —
  this only keeps the door open.
- **`coredump` 64 K → 128 K.** A dual-core panic with Wi-Fi, NimBLE, an HTTP server and TinyUSB
  live can exceed 64 K and silently truncate. This device has no other debug channel once it is
  headless.

App slots stay at 4 M each even though a realistic fully-loaded build is 1.5–2.5 M. The slack
is what buys "never repartition again", which is the whole point of getting this right once.

**Bulk capture data belongs on the SD card, not `littlefs`** — NOR flash has finite erase
cycles and the card is 128 GB and replaceable. §4 routes `wifiscan`/`blescan` output to SD
accordingly.

Dual OTA is the point of the exercise: after the first flash, every iteration should go over the
air from the phone, and a bad build should roll back instead of bricking. **Half of that is
built.** The rollback *confirmation* is implemented and described below; the *delivery* path —
anything that writes an image into the inactive slot — does not exist yet (backlog S5), so
updates are still a USB flash today.

**Guard rail — done, and re-proven after the fact.** `restore.sh --yes` was first run on
2026-08-15 while the device was still factory-fresh. It was then run again as a full round-trip
*from the repartitioned layout*: restore → factory demo boots → reflash W1 → console responds.
That second run is the one that counts, because it is the only evidence `restore.sh` can
actually overwrite our 16 MB-header bootloader and 7-entry table with the factory 8 MB-header,
5-entry ones. Recovery is now demonstrated, not merely reasoned about.

### Repartitioning is NOT a standalone step

Decided 2026-08-15 after decoding the factory images:

```
bootloader.bin  header: flash size = 8MB   (chip is actually 16MB)
app0.bin        header: flash size = 8MB
```

The ESP-IDF bootloader validates partition entries against the flash size in its own header and
rejects any that overrun it. The planned `littlefs` at `0x810000–0xff0000` lies entirely beyond
8 MB, so writing the new table under the **factory** bootloader would likely refuse to boot —
not merely lose SPIFFS.

It is fixable (flash the bootloader with `--flash-size 16MB` instead of `keep`, which makes
esptool rewrite the header and recompute the image hash), but not worth doing on its own:

- A partition table is inert. It only means anything once firmware uses it.
- Repartitioning now costs the working factory demo — its `spiffs` partition ceases to exist —
  and buys nothing until `app0` has our own app in it.
- PlatformIO writes bootloader + partition table + app in a single flash anyway. Our own
  bootloader will carry a 16 MB header, so the repartition falls out of the first real flash
  for free.

**So: the repartition happens as part of the W0 firmware flash, not before it.**

---

## 4. Feature areas

### Platform
Partition scheme · LittleFS · OTA with rollback · NVS config store · factory reset via boot
button (GPIO 0) · event bus · logging · LCD status page · APA102 status colour (GPIO 40/39)

### Transport
Wi-Fi SoftAP · Wi-Fi STA + mDNS · captive portal · HTTP server + WebSocket · BLE GATT
(NimBLE) · BLE Wi-Fi provisioning · **auth**

Auth is not optional. An open AP exposing keystroke injection into the host PC is a genuine
liability. Minimum viable: a per-device PIN shown on the LCD, exchanged for a session token,
with the AP running WPA2 using a key derived at first boot. The LCD is a real security asset
here — it gives us an out-of-band channel most IoT devices lack.

### Tool modules

| Module | Claims | Notes |
|---|---|---|
| `cdc` | usb: shared | The serial console itself, `essential` — always on, cannot be disabled. |
| `hid` | usb: shared | Keyboard + mouse injection, macro playback from LittleFS/SD. Composite with CDC so the serial console survives. **`bootTimeBinding`** — arming it requires a reboot. Never default-enabled. |
| `msc` | usb: shared, sd: **exclusive** | Expose the SD card to the host PC as a drive. Locks out `storage` by claim; exclusion with `hid` is module policy, not a claim. Also `bootTimeBinding`. |
| `storage` | sd: shared | Browse / upload / download the card from the phone. **Card must stay FAT32** (see CLAUDE.md). |
| `wifiscan` | wifi: shared (**exclusive** in monitor mode) | AP survey, RSSI, channel occupancy, log to SD. |
| `blescan` | ble: shared | Device scan, beacon advertise, presence logging. Unaffected by Wi-Fi monitor mode. |
| `display` | lcd: exclusive | Push text/images to the 160×80 ST7735 from the phone. |
| `led` | led: exclusive | APA102 status colour / heartbeat. The W1 reference module. |
| `gpio` | — | Thin on the base model: only GPIO 43/44 are broken out. |

Notably absent: IR, microphone, QWIIC — those are Plus-variant hardware this board does not have.

#### The USB mode switch `hid`/`msc` will force

The S3 has **two** USB peripherals sharing one PHY, and they are mutually exclusive at runtime:

- `ARDUINO_USB_MODE=1` — the fixed-function native **USB-Serial/JTAG** block. What W0 uses, and
  what the factory firmware enumerated as (`303a:1001`). Gives JTAG debug over the same cable.
- `ARDUINO_USB_MODE=0` — **TinyUSB** over the OTG controller. The only way to get composite
  CDC+HID+MSC, which `hid` and `msc` both need. Changes the USB PID (LilyGO's own board JSON
  anticipates `303a:82c1`) and gives up USB-JTAG.

So enabling `hid` or `msc` is not just a module toggle — it is a build-time USB mode change that
alters how the device enumerates on the host.

**Resolved 2026-08-16: TinyUSB (`ARDUINO_USB_MODE=0`) is the primary build.** Both modes were
built and flashed to the real device and the trade-off measured rather than estimated:

| | MODE=1 (USB-Serial/JTAG) | MODE=0 (TinyUSB) |
|---|---|---|
| Static RAM | 23,040 B | 55,208 B (**+32 KB**) |
| Flash | 322,486 B | 376,582 B (+54 KB) |
| Free heap at idle | 345,592 B | 308,336 B (**−37 KB**) |
| Largest free block | 286,708 B | 258,036 B |
| Deploy loop | ~10.0 s | ~12.2 s |
| Composite CDC+HID+MSC | impossible | available |
| USB-JTAG | available | **gone** |

Losing USB-JTAG costs nothing recoverable elsewhere: S3 JTAG is on GPIO 39–42, and on this board
39/40 are the APA102 while 41/42 are not broken out — an external probe was never possible. The
`coredump` partition carries post-mortem duty instead.

**The 37 KB of heap is the number to watch.** That is spent before `hid` or `msc` do anything,
on a board with no PSRAM, and Wi-Fi + NimBLE + an HTTP server still have to fit.

`ARDUINO_USB_MODE` is a preprocessor `#if` selecting the `Serial` class
(`HardwareSerial.h:442`) — there is **no runtime or boot-time switch**. The `t-dongle-s3` env is
retained as a reflash-away fallback for any session that wants JTAG.

One trap this uncovered: under TinyUSB, esptool's DTR/RTS reset is implemented in firmware, so
`pio run -t upload` fails with `No serial data received` — measured 3/3. A 1200-baud touch
(`scripts/touch_reset.py`, wired into the env) drops it into the ROM bootloader and restores a
fully automated flash loop.

#
**The Arduino core will confirm the image for you unless you stop it.** `initArduino()`
(`cores/esp32/esp32-hal-misc.c:315`) runs before `setup()` and, under
`CONFIG_APP_ROLLBACK_ENABLE`, calls `esp_ota_mark_app_valid_cancel_rollback()` itself via a
**weak** `verifyOta()` that returns `true` unconditionally. Out of the box the rollback window
therefore never exists: an image is confirmed before any application code runs, and a health
check added later can only ever observe `VALID`.

`otahealth.cpp` overrides the framework's own hook — `extern "C" bool verifyRollbackLater()`
returning `true` — which tells the core to leave the image `PENDING_VERIFY` and let the
application decide. Without that one function every other line of the health check is dead code.

This was found on hardware, not in review: 194 host tests passed against logic that could never
execute. The symptom was an image booting straight to `VALID` with `phase: idle` at 11 s uptime,
long before the 30 s gate could have confirmed anything.

### OTA rollback — armed by the bootloader, confirmed by the app

**Corrected 2026-08-17.** Backlog S4 claimed `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` was not
set. It is, and always has been:
`~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig` line 424
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`) and line 4392 (`CONFIG_APP_ROLLBACK_ENABLE=y`).
We flash the **prebuilt** bootloader from that package — `bin/bootloader_qio_80m.elf`, which
carries the symbol `set_actual_ota_seq`, compiled only under that option.

That made the real bug worse than "rollback doesn't work". The bootloader was *armed* and the
app never confirmed itself, so the first image delivered by OTA would have landed in
`ESP_OTA_IMG_PENDING_VERIFY`, appeared to work, and been reverted by the bootloader at the next
restart — with nothing anywhere connecting the two events. It had never bitten only because a USB
flash never produces `PENDING_VERIFY`: PlatformIO writes
`framework-arduinoespressif32/tools/partitions/boot_app0.bin` at otadata's offset, and that file
is **not blank** — sector 0 carries one valid entry (`ota_seq = 1`, `ota_state = 0xFFFFFFFF`
i.e. `UNDEFINED`, `crc = 0x4743989a`) and sector 1 carries `ota_seq = 0`, which is invalid by
definition. `ota_seq 1` selects slot `(1-1) % 2` = app0, and `UNDEFINED` is the state the
bootloader boots without limits. It is byte-for-byte identical to
`backup/factory_release/otadata.bin`. That is why `info` reports `UNDEFINED` rather than nothing.

This is **app-side only**. No `sdkconfig.defaults`: adding one switches the project to a
from-source ESP-IDF build, costs the 12-second deploy loop and changes which `boot_app0` code
path runs (see `firmware/scripts/check_boot_app0.py`).

**What is implemented** — `firmware/src/otadecide.h` (pure, host-tested) and
`firmware/src/otahealth.{h,cpp}` (the flash/registry/clock half), driven by an `ota.health`
scheduler task at 250 ms:

- On boot, read `esp_ota_get_state_partition()` for the running partition. Anything other than
  `PENDING_VERIFY` — which is every USB flash — parks in `idle` and never acts again.
- On `PENDING_VERIFY`, run a health check and call `esp_ota_mark_app_valid_cancel_rollback()`
  only when **all five** criteria hold:

  | criterion | threshold | what it proves |
  |---|---|---|
  | `ticks` | 40 runs of the health task (≈10 s) | the cooperative scheduler is dispatching, not wedged |
  | `registry` | `ModuleRestoreReport::nvsTooLong` is false | the module registry restored without its one fatal outcome |
  | `essential` | `cdc` is enabled | the console — the only guaranteed link to the device — is up |
  | `console` | one request answered on **any** transport, **or** 20 s elapsed | it answers, or nobody is asking |
  | `uptime` | 30 s | a crash loop cannot reach its own mark-valid call |

- **Wi-Fi is deliberately not a criterion.** `http` is opt-in with `defaultEnabled = false`, so
  requiring the AP would roll back a perfectly good build on a device whose owner has the radio
  off — which is the factory default. The same reasoning excludes `storage` (no card is legal,
  and there is no card-detect pin), `display` and `hid`.
- **The window is 180 s** (6× the uptime gate). If the criteria are still outstanding then, the
  app calls `esp_ota_mark_app_invalid_rollback_and_reboot()` rather than sitting in
  `PENDING_VERIFY` indefinitely — because the bootloader would roll back anyway at whatever
  restart happened next, and the owner would experience that as "the update vanished" days
  later. Deciding inside the window at least makes it an event.
- Confirmation and rollback both emit on the bus — `ota.pending`, `ota.confirmed`,
  `ota.rollback`, `ota.confirm_failed` — so they are visible on every transport and in the boot
  log, with the criteria mask expanded to named booleans rather than a number.
- The LCD costs nothing extra: `otahealth.cpp` reports through `activity.h`, so the existing
  footer renders `ota verify NN%` on both screens with no new region and no change to
  `mod_display.cpp`.

**The `ota` built-in** exposes and overrides all of it. No params: running partition, the
partition `otadata` will boot *next*, state, whether confirmation is pending, each criterion,
seconds left in the window, and the rollback target — reported as *two separate facts*,
`has_app` (a valid image magic word is present) and `rollback_possible` (`otadata` actually
blesses it as bootable). `p:{confirm:true}`, `p:{rollback:true}` and `p:{boot:"<label>"}` are
the three mutating parameters and all three are **`AUTH_PHYSICAL`**; `rollback` refuses,
naming which of the two facts is missing, rather than rolling into an erased slot.

`p:{boot:"app0"|"app1"}` (bare-word `ota boot app1`) is `esp_ota_set_boot_partition()` on a
partition looked up **by label among app partitions only** — never an offset, because a
hand-typed address turns a typo into a reflash. It **does not reboot**: selecting the next
image and restarting into it are separate decisions, and `reboot` is the second one. It
refuses, before `otadata` is touched, a malformed label, a label that names nothing, a label
that names a *data* partition, and — the one that matters — an app partition with no valid
image, using the same `esp_ota_get_partition_description()` check `rollback_target.has_app`
reports. IDF then applies a stronger gate of its own (full `ESP_IMAGE_VERIFY`, so a truncated
slot is refused too). The response carries previous and selected labels plus the selected
image's build date and `idf_ver`, so the caller can see *which* image it just chose, and
`ota.boot_set` goes on the bus with the old/new pair. The decision table
(`OtaDecide::decideBootSet()`) is host-tested: every refusal branch ends on hardware in either
"reboots into the other build" or "USB-recovery job", and only one of those is reachable safely
on the bench.

**This is the primitive S5's delivery path will use** to select the freshly written slot once
it has finished writing it. It is also what makes the rollback machinery above testable today:
`esp_ota_set_boot_partition()` writes the target's `otadata` entry with
`ota_state = ESP_OTA_IMG_NEW` (verified in the disassembly of the prebuilt
`libapp_update.a` — `movi a9, 0; s32i.n a9, a8, 24`, i.e. `ota_state = 0` on the inactive
`otadata` sector), and with rollback armed in the bootloader that becomes a `PENDING_VERIFY`
boot — the state a USB flash can never produce.

`ota` is the first built-in whose *parameters* are gated above its row in `CmdAuth::BUILTINS`
(reads at `TOKEN`, mutates at `CmdAuth::OTA_MUTATE == PHYSICAL`). The read stays at `TOKEN` on
purpose: once S5 lands, the client that pushed an update is the one that needs to see whether it
was confirmed.

**Still missing (backlog S5):** there is no OTA delivery path at all — nothing calls
`esp_ota_begin`/`esp_ota_write`, and no transport accepts an image, so getting a build into the
inactive slot is still a USB `esptool write-flash`. What is no longer missing is the last step
of that path: `ota p:{boot:...}` selects the slot, so the confirmation machinery can be
exercised end to end without hand-editing `otadata`.

### UI
Phone web app (module cards, toggles, per-module panels, WS live data) · on-device LCD (mode,
IP, PIN, activity)

---

## 5. Build stack

**Decided 2026-08-15: PlatformIO + Arduino-ESP32 3.x via the `pioarduino` fork**, with
**NimBLE-Arduino** and **TinyUSB** for composite CDC+HID+MSC.

The fork is required, not a preference. Official `platformio/espressif32@7.0.1` ships
`framework-arduinoespressif32 ~3.20017.0` — that is Arduino-ESP32 **2.0.17** on IDF 4.4.x.
PlatformIO never shipped official Arduino 3.x support; it lives in
`github.com/pioarduino/platform-espressif32`. Arduino 3.x matters here because the `hid` and
`msc` modules need modern TinyUSB composite support, which the 2.0.x USB stack makes painful.

Trade-off accepted: the fork is community-maintained. Pin an explicit release rather than
tracking a branch.

Why not raw ESP-IDF: the LCD and SD paths are already solved in LilyGO's Arduino examples, and
Arduino 3.x exposes the IDF APIs we need (partitions, LittleFS, `esp_http_server`, TinyUSB)
directly. Escape hatch if we hit a wall: Arduino-as-an-IDF-component keeps both.

There is no official `lilygo-t-dongle-s3` board definition in PlatformIO, so the project carries
its own at `firmware/boards/lilygo-t-dongle-s3.json` (16 MB, no PSRAM).

### Toolchain notes (this machine)

- The pre-existing `~/.platformio/penv` was **broken** — built against Python 3.10, stranded by
  the upgrade to 3.12. Replaced with `uv tool install platformio` (PlatformIO 6.1.19 at
  `~/.local/bin/pio`), which is immune to system Python bumps.
- **`uv tool install platformio` needs `--with pip`.** PlatformIO shells out to `pip` to install
  esptool's Python dependencies into `tool-esptoolpy`; a uv venv has no pip by default, and the
  package silently unpacks without its `package.json`, producing a misleading
  `MissingPackageManifestError` and an equally misleading "sudo apt install python3-dev
  libffi-dev libssl-dev" hint. Neither is the real cause.

---

## 6. Divide and conquer

Layered, because the dependencies are real. Within a layer, work is genuinely parallel.

**W0 — Platform & flash** *(blocking; nothing else is safe to iterate on first)*
Repartition, LittleFS, OTA + rollback, NVS config, factory reset, verify the factory restore
path still works.

**W1 — Core contract** *(blocking for W2/W3)*
Command bus, module registry, resource claims, auth. This is the file everyone else compiles
against — get the descriptor schema right before the fan-out.

**W2 — Transports** *(parallel: CDC ‖ HTTP/WS ‖ BLE)*

**W3 — Tool modules** *(parallel: hid ‖ msc ‖ storage ‖ wifiscan ‖ blescan ‖ display)*

**W4 — Web UI** — can start against a mocked `/api/modules` the moment W1's schema is fixed.

**W5 — LCD/LED status** — small, and can slot in anywhere after W0.

### First vertical slice

Repartition → CDC transport → registry → **one trivial module (`display` or the LED)**.

That exercises the whole spine end to end over USB alone, with no radio and no web UI in the
way, and it is cheap to throw away if the shape turns out to be wrong. Only once commands flow
cleanly over CDC do we add HTTP/WS, then BLE — each new transport should be a pure addition
with zero changes to the modules.
