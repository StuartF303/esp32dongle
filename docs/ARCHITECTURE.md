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

### Auth is part of the descriptor (backlog S6/S7/S8, done 2026-08-17)

The descriptor carries a `minAuth` and each `ModuleAction` carries its own, both initialised at
compile time from **`firmware/src/modauth.h`** — the module-side twin of `cmdauth.h`, and
dependency-free for the same reason: the native test env excludes `src/`, so a policy living in
`mod_*.cpp` could never be asserted on the host. `Registry::dispatch()` enforces it centrally via
`ModAuth::decide()`, and the modules' own hand-rolled checks (`hid`'s `requireInjectAuth`,
`storage`'s `requireAuth`/`requirePhysical`, `http`'s `requirePhysical`, `display`'s backlight
check) are gone.

- **The module's bar is checked before ENOMOD / EDISABLED / EREBOOT.** Those three are facts
  about the module map — including "armed, binds at boot", i.e. *the keyboard is coming back at
  the next restart* — and a caller below the lowest module bar gets one refusal that names
  nothing, so a real id and an invented one are indistinguishable. That is exactly what S1 did
  for the built-ins with `CmdAuth::minimumLevel()`, and it is what lets BLE dispatch at
  `AUTH_NONE`.
- **`Registry::list()` filters by the caller's level** and emits `min_auth` per module plus
  `min_auth` + `allowed` per action, so the phone UI greys out what the session cannot use
  instead of discovering it by failure. A module below the caller's bar is absent entirely — not
  rendered as forbidden, which would answer the question the bar exists to refuse.
- **Levels:** everything is `AUTH_TOKEN` except `storage.format`, `http.psk` and `http.pin`,
  which stay `AUTH_PHYSICAL`. Nothing is at `AUTH_NONE`, and there is no way to spell it: an
  undeclared level is 0, which `ModAuth::effective()` reads as `AUTH_PHYSICAL`, `allGated()`
  turns into a build failure, and a `static_assert` forbids in the table.
- **One capability, one bar.** The `led` built-in is a pure alias for `led.set`, so console.cpp
  carries a `static_assert` that `CmdAuth::requiredFor("led") == ModAuth::requiredFor("led",
  "set")`. Changing either alone fails the build.

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
air from the phone, and a bad build should roll back instead of bricking. **Both halves are now
built** — the rollback *confirmation* (S4) and the *delivery* path (S5, `POST /api/ota`), both
described below.

`littlefs` is mounted at boot by `firmware/src/fsmount.{h,cpp}` as platform infrastructure, in
the same class as NVS rather than as a module — so nothing has to be enabled for the web assets
to be readable. It is **never auto-formatted**: an unformatted partition and a corrupt one are
indistinguishable at the mount call, so a failure is reported and `storage format` (AUTH_PHYSICAL)
is the explicit remedy. `storage` reaches both filesystems through one surface with a volume
prefix — `/sd/...` and `/fs/...` — see §4.

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

#### Pairing model — decided 2026-08-24 with stuart

The AP is **single-client** and there is **one session**; `AP_MAX_CLIENTS` and `MAX_SESSIONS`
both go to 1. The pairing PIN drops from 8 digits to **4**, and stops being a stored credential:

- **RAM only.** The NVS `pin` key goes away. A PIN that is regenerated on every boot has no
  reason to survive one, and not writing it removes the flash-wear vector `ratelimit.h` warns
  about.
- **Minted on power-up, on session end, and on lockout.** Session end is 90 s after the single
  AP client disassociates, or an explicit unpair. Lockout is the rate limiter's 10-failure trip.
- **Consumed on use.** The `Pairing::shouldShow()` policy is unchanged and now describes the
  whole lifetime: the PIN is on the LCD exactly while the device is pairable.

**Why 4 digits is defensible and 4 digits alone would not be.** The limiter allows about 34
guesses an hour (1, 2, 4, 8, 16, 30, 30… s, then 15 minutes), so a static 4-digit PIN falls in
about six days of grinding — a bounded search, which is the objectionable part. Minting a new PIN
on every lockout makes each 17.5-minute cycle cover 10/10⁴ of a *fresh* space: the attacker never
accumulates progress. Single-client association is the second half — while the phone holds the
one slot nobody else can associate at all, so the brute-force window exists only while the device
is unpaired. The gain is legibility: 4 digits render at roughly twice the height on a 160×80
panel that is often read at arm's length from behind a desktop machine.

**What it costs.** An attacker in radio range can squat the single AP slot and deny pairing, with
no recovery except USB. That DoS already existed under S3's public passphrase — a squatter
grinding the PIN was already occupying a slot — so rotation on lockout adds no new exposure.

**The lockout this design had to avoid**: single PIN plus single session plus consume-on-use means
a phone that silently drops the AP leaves a session slot held by a token the browser no longer
has, and no PIN left to re-pair with. The 90 s grace period is the fix, and it is only coherent
because the AP is single-client: the association *is* the session boundary. Inside the window the
page reconnects from `localStorage` with no user action; outside it, revoke and re-mint.

#### QR pairing on the LCD

Two codes, because no single QR can both join a network and open a page: a `WIFI:` join payload
(needed once — the phone remembers the network) and a pair URL carrying the current PIN in a path
segment, `HTTP://192.168.4.1/<pin>`.

The path segment rather than a query string is deliberate: `?` and `=` are not in QR alphanumeric
mode but `:` `/` `.` and uppercase are, so `HTTP://192.168.4.1/4821` (23 chars) encodes as
**version 1, 21×21**, where a query-string form needs byte mode and version 2. Scheme case is
irrelevant to every browser.

Fit on the 160×80 panel — height is the only constraint, and the pitch is 0.136 mm (0.96 inch
diagonal, 178.9 px across it):

**Measured, not estimated** — run through the vendored encoder itself (`firmware/lib/qrcodegen`,
`qrcodegen_encodeText` at ECC LOW with `boostEcl`, which is what the firmware will call), against
the real SSID format `tdongle-%02x%02x` and the real generated passphrase form:

| payload | ch | version | modules | scale @ quiet 4 | module |
|---|---|---|---|---|---|
| `HTTP://192.168.4.1/4821` | 23 | **1** | 21×21 | 2 px → 58 px | 0.27 mm |
| `http://192.168.4.1/4821` (lowercase) | 23 | 2 | 25×25 | 2 px → 66 px | 0.27 mm |
| `http://192.168.4.1/?p=4821` | 26 | 2 | 25×25 | 2 px → 66 px | 0.27 mm |
| `WIFI:` + generated 15-char PSK | 45 | **3** | 29×29 | 2 px → 74 px | 0.27 mm |
| `WIFI:` + a 63-char owner-set PSK | 93 | 5 | 37×37 | **1 px → 45 px** | **0.14 mm** |

The first four fit with a spec-compliant quiet zone. At 2 px/module a module is 0.27 mm, which a
12 MP phone at 10 cm resolves at ~8 camera pixels — far above the ~3 px threshold. Version 1 at 3
px/module with a 2-module quiet zone is 75 px and gives 0.41 mm modules; that is the robust
option if the panel turns out to smear adjacent modules. **Decide between them by scanning both
off the real glass, not by arithmetic** — the risks here are specular glare and ST7735 pixel
bleed, not resolution.

The uppercase scheme is worth a whole version and costs nothing: URL schemes are case-insensitive
to every browser, and `HTTP://192.168.4.1/4821` is entirely inside QR alphanumeric mode
(`0-9 A-Z $%*+-./: `), where the lowercase form falls back to byte mode.

**The last row is the rule that had to be measured.** `psk set` accepts any legal WPA2
passphrase up to 63 characters, and a long one pushes the join code to version 5, where 80 pixels
of panel leaves 1 px per module — 0.14 mm, below what a phone can resolve. So the renderer
**refuses to draw below 2 px/module and prints the SSID and passphrase as text instead**. An
unreadable QR is worse than no QR: it looks like it should work.

Security is unchanged by the QR: a camera needs line of sight to the screen, the same out-of-band
property as reading the digits. The QR is therefore governed by the same `Pairing::shouldShow()`
predicate and must never be reachable through `display.screen` over the wire. The page strips the
PIN from the URL with `history.replaceState` on load, so a reload cannot re-pair.

#### Answering the captive-network probes — decided 2026-08-24 with stuart

Both phone platforms probe for internet the moment they associate, and both react badly when the
probe fails: iOS raises the Captive Network Assistant sheet over whatever you were doing, and
Android marks the network as having no connectivity, warns, and **may silently move back to
mobile data** — which drops the association, which under the model above ends the session 90
seconds later. The 90 s grace window exists partly to survive that. Not provoking it is better
than surviving it.

So the device runs a **DNS responder that answers every query with 192.168.4.1**, and replies to
the two well-known probes with exactly what a working connection returns:

| probe | expected |
|---|---|
| `http://captive.apple.com/hotspot-detect.html` (and the other Apple hosts) | `200` with the body `<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>` |
| `http://connectivitycheck.gstatic.com/generate_204` (and `clients3.google.com`, `www.google.com/generate_204`) | `204` with an empty body |

The DNS responder is required, not optional: the probes are fetched **by hostname**, so without
one they never reach this device at all and the answer is moot.

**This is deliberately a lie, and it is recorded as one.** The device is telling the phone that a
network with no route anywhere has working internet. The justification is that the user joined
this AP on purpose, to drive a tool, for a few minutes at a time — the OS warning is protecting
against a case that does not exist here, and the cost of it firing is a dropped session. The
honest alternative (a full captive portal that redirects the probe to the pairing page) was
considered and rejected: the CNA sheet is a restricted browser with no durable `localStorage`, so
the session token would not survive it, and Android would still treat the network as internetless
and still consider leaving. It optimises first contact — which the pair QR already solves — at
the cost of every reconnect after it.

**mDNS was considered and deferred** (backlog F8). The QR carries the IP, so a name buys little,
and mDNS wants 5–10 KB of heap on a board where TinyUSB already took 37 KB and BLE (F1) still
needs ~40 KB of the ~145 KB free with Wi-Fi up.

### Tool modules

| Module | Claims | Notes |
|---|---|---|
| `cdc` | usb: shared | The serial console itself, `essential` — always on, cannot be disabled. |
| `hid` | usb: shared | Keyboard + mouse injection, macro playback from LittleFS/SD. Composite with CDC so the serial console survives. **`bootTimeBinding`** — arming it requires a reboot. Never default-enabled. |
| `msc` | usb: shared, sd: **exclusive** | Expose the SD card to the host PC as a drive. Locks out `storage` by claim; exclusion with `hid` is module policy, not a claim. Also `bootTimeBinding`. |
| `storage` | sd: shared | Browse / upload / download **both** filesystems from the phone: `/sd/...` (microSD, **must stay FAT32** — see CLAUDE.md) and `/fs/...` (LittleFS). One chunked read/write/CRC/list/stat/mkdir/delete/verify surface, one `PathSafe` funnel. Enables on **either** volume, so an absent card does not take LittleFS with it; it still claims `sd` shared either way, because claims are static. `format` (`/fs` only) is **AUTH_PHYSICAL**. |
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

### OTA delivery — `POST /api/ota` (backlog S5, done 2026-08-17)

`firmware/src/otaupload.{h,cpp}` streams an image body straight into the inactive slot with
`esp_ota_begin` / `esp_ota_write` / `esp_ota_end`. **Nothing is staged in LittleFS first**: that
would need 1.25 MB of the 7.75 MB partition, write every byte to NOR flash twice, and add a
failure mode between "received" and "installed". The inactive app slot already *is* the safe
place for an unverified image, and `esp_ota_end()` validates it before anything can select it.

The transport half is `handleOta()` in `mod_http.cpp`:
`POST /api/ota?len=<bytes>[&sha256=<64 hex>][&select=1]`, **`AUTH_TOKEN`**.

- **The size is declared up front and enforced twice** — against `Content-Length` before a
  sector is erased, and against the bytes actually received. A short body is `ESHORT`, not a
  truncated image. Chunked transfer-encoding is refused (`ELENGTH`) rather than read until it
  stops.
- **Optional SHA-256, checked before `esp_ota_end()`**, because a corrupt-but-well-formed image
  passes IDF's validation. The digest of what was actually received is reported either way, so
  an upload is verifiable against `sha256sum firmware.bin` after the fact.
- **Bounded everywhere**: a 4 KB transfer buffer (malloc'd per upload, freed on every path), a
  hard cap at the slot size, a 15 s stall timeout and a 300 s total deadline. Every failure path
  runs `esp_ota_abort()`, and **nothing on any failure path touches `otadata`** — so a partial
  upload leaves the running image selected and the half-written slot inert.
- **It never reboots.** `?select=1` calls the same `OtaHealth::setBootNow()` that `ota boot`
  uses — which is what writes `ota_state = ESP_OTA_IMG_NEW` and therefore **arms the rollback
  machinery above**: the next boot of that image is `PENDING_VERIFY`, `verifyRollbackLater()`
  keeps it there, and the five criteria decide. Restarting is a separate, `AUTH_PHYSICAL`
  decision (`reboot`).
- Progress goes to the LCD through `activity.h` (`ota upload NN%`, reusing what `otahealth`
  already does) and to the bus as `ota.upload.begin` / `.progress` / `.done`. `ota` reports an
  upload in flight or the last result under `d.upload`.
- Distinct codes throughout: `EBUSY` `EARGS` `ELENGTH` `ETOOSMALL` `ETOOBIG` `ENOSLOT`
  `EPENDING` `ECONFLICT` `ENOMEM` `EMAGIC` `ESHORT` `ECONN` `ETIMEOUT` `ESHA256` `EIMAGE`
  `EOTABEGIN` `EOTAWRITE` `EOTAEND` `ESELECT` `ESTOPPING`. `EPENDING` is the one that will be
  met in practice: IDF refuses `esp_ota_begin()` while the running image is still
  `PENDING_VERIFY`, which is exactly the first ~30 s after an OTA boot.

**The security consequence, decided by stuart on 2026-08-17 and recorded in full above
`handleOta()`:** upload *and* activation at `AUTH_TOKEN` make a session token equivalent to
arbitrary code execution on this device, and combined with the deliberately public AP passphrase
(backlog S3) that puts the security boundary at radio range. He was told this plainly and chose
it, for true over-the-air updates from a phone. `ota confirm` / `rollback` / `boot` stay
`AUTH_PHYSICAL`; this is a new path, not a widening of those.

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
