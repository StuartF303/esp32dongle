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
{"id": 7, "ok": false, "e": {"code": "EBUSY", "msg": "USB claimed by msc"}}
// event
{"ev": "wifiscan.result", "d": {"ssid": "...", "rssi": -62}}
```

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
  .id        = "hid",
  .name      = "USB Keyboard",
  .category  = CAT_INPUT,
  .claims    = CLAIM_USB,          // bitmask of exclusive resources
  .enable    = hid_enable,
  .disable   = hid_disable,
  .dispatch  = hid_dispatch,
}
```

`GET /api/modules` returns the descriptor list, and the web UI renders itself from that — so a
new module needs **zero** front-end changes.

**Two levels of gating, both needed:**

- **Compile-time** (`menuconfig` / build flags) — what is in the binary at all. Flash and RAM
  are the budget.
- **Runtime** — what is active now, persisted in NVS, toggled from the phone.

Runtime claims are what make this more than cosmetic. `CLAIM_USB` means HID and MSC cannot both
be on; `CLAIM_SD` means MSC (which hands the card to the host PC) locks out the storage browser;
`CLAIM_RADIO_EXCL` means Wi-Fi monitor mode can't run while it's your only link to the device.
The registry refuses the impossible combination with a clear error instead of hanging.

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

Dual OTA is the point of the exercise: after the first flash, every iteration goes over the air
from the phone, and a bad build rolls back instead of bricking.

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
| `hid` | USB | Keyboard + mouse injection, macro playback from LittleFS/SD. Composite with CDC so the serial console survives. |
| `msc` | USB, SD | Expose the SD card to the host PC as a drive. Mutually exclusive with `hid` and `storage`. |
| `storage` | SD | Browse / upload / download the card from the phone. **Card must stay FAT32** (see CLAUDE.md). |
| `wifiscan` | RADIO | AP survey, RSSI, channel occupancy, log to SD. Monitor mode needs `RADIO_EXCL`. |
| `blescan` | RADIO | Device scan, beacon advertise, presence logging. |
| `display` | LCD | Push text/images to the 160×80 ST7735 from the phone. |
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
alters how the device enumerates on the host. Plan for it in W1 rather than discovering it in W3.

#### OTA rollback is not automatic

Two OTA slots in the partition table do not by themselves give rollback. It also needs
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` and the app calling
`esp_ota_mark_app_valid_cancel_rollback()` once it has confirmed itself healthy — e.g. after
Wi-Fi and the HTTP server are up. Until that call exists, a bad OTA does *not* roll back.

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
