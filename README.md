# esp32dongle

Custom firmware for a **LilyGO T-Dongle-S3** — a USB-stick-sized ESP32-S3 board with a 160×80
colour LCD and a microSD slot hidden inside the USB-A shell.

It turns the dongle into several tools at once, all controlled from a phone over the dongle's own
Wi-Fi access point:

- **USB keyboard** — types on whatever computer it is plugged into, with a proper en_GB layout
- **File bridge** — the microSD card and 7.75 MB of internal flash, over a chunked, checksummed API
- **Wi-Fi AP + web UI** — PIN-paired, serving the control interface
- **Status LCD** — device-owned, showing pairing PIN, network state and live activity
- **OTA updates** — dual-slot, with a health check and automatic rollback

Everything is driven by a **transport-agnostic command bus**: modules describe themselves, and
every transport (USB serial, HTTP, WebSocket, and BLE later) speaks the same JSON protocol. The web
UI renders itself from those descriptors, so a new module needs no front-end changes.

---

## Layout

| Path | |
|---|---|
| `firmware/` | The ESP32-S3 firmware. PlatformIO + Arduino-ESP32 via the pioarduino fork. |
| `firmware/tools/console.py` | Host-side console for driving the device over USB. |
| `docs/ARCHITECTURE.md` | Design, decisions and the reasoning behind them. |
| `docs/BACKLOG.md` | Deliberately deferred work, with *why* it was deferred. |
| `design/` | Handoff brief and real captured API payloads for UI/UX work. |
| `hardware/` | Schematic. |
| `backup/factory_release/` | Verified image of the stock LilyGO firmware — see its README. |
| `CLAUDE.md` | Working notes and verified hardware facts. Read this before touching the device. |

## Build and flash

```bash
pio run -d firmware              # build
pio run -d firmware -t upload    # flash (this REPARTITIONS a stock device)
pio test -e native               # 229 host tests, no hardware needed
```

Then talk to it:

```bash
uv run --with pyserial python firmware/tools/console.py info
uv run --with pyserial python firmware/tools/console.py modules
```

There are two build environments: `t-dongle-s3-tinyusb` (default — composite USB, needed for the
keyboard) and `t-dongle-s3` (the fixed-function USB-Serial/JTAG fallback, the only way to get
USB-JTAG on this board).

**Read `CLAUDE.md` before flashing.** This board has real gotchas — esptool's stub flasher fails on
it, the TinyUSB build needs a 1200-baud touch to enter download mode, and the microSD must stay
FAT32.

## Hardware

ESP32-S3 (no PSRAM), 16 MB flash, repartitioned to a dual-OTA layout. ST7735 LCD on SPI2, microSD
on SDMMC 4-bit, APA102 RGB LED. Verified pinouts and the full flash map are in `CLAUDE.md` and
`docs/ARCHITECTURE.md`.

## Status

Working: the command bus and module registry with resource arbitration, USB HID keyboard, storage
across two volumes, the Wi-Fi AP with PIN-paired sessions, the status LCD, and OTA rollback.

Not yet: BLE transport, USB mass storage, radio survey tools, stored macros, and a designed web UI
— see `docs/BACKLOG.md`, which also lists the known gaps and accepted trade-offs.

## Credits

The stock firmware backup and the board's pinout come from LilyGO's own
[T-Dongle-S3 repository](https://github.com/Xinyuan-LilyGO/T-Dongle-S3). See
`backup/factory_release/README.md` for attribution.
