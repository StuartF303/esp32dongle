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
| `0x9000` | 20 K | nvs | config, module enable state, Wi-Fi creds |
| `0xe000` | 8 K | otadata | |
| `0x10000` | 4 M | app0 (ota_0) | |
| `0x410000` | 4 M | app1 (ota_1) | OTA target + rollback |
| `0x810000` | ~7.9 M | littlefs | web assets, macros, logs, captures |
| `0xff0000` | 64 K | coredump | |

Dual OTA is the point of the exercise: after the first flash, every iteration goes over the air
from the phone, and a bad build rolls back instead of bricking.

**Guard rail:** the very first thing to verify after any repartition is that
`backup/factory_release/restore.sh` still puts the device back. Test the escape hatch before
relying on it.

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

### UI
Phone web app (module cards, toggles, per-module panels, WS live data) · on-device LCD (mode,
IP, PIN, activity)

---

## 5. Build stack

Recommendation: **PlatformIO + Arduino-ESP32 3.x** (which is ESP-IDF 5.1 underneath), with
**NimBLE-Arduino** and **TinyUSB** for composite CDC+HID+MSC.

Why not raw ESP-IDF: the LCD and SD paths are already solved in LilyGO's Arduino examples, and
Arduino-ESP32 3.x exposes the IDF APIs we need (partitions, LittleFS, `esp_http_server`,
TinyUSB) directly. Escape hatch if we hit a wall: Arduino-as-an-IDF-component keeps both.

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
