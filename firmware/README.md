# firmware — W0 platform skeleton

PlatformIO project for the LilyGO T-Dongle-S3, built against the **pioarduino**
fork of `platform-espressif32` (Arduino-ESP32 3.x / ESP-IDF 5.x). The official
`espressif32` platform is stuck on Arduino-ESP32 2.0.17 and can't be used here.

W0 (per `../ARCHITECTURE.md` section 6) proved the toolchain, the new 16 MB
dual-OTA partition table, and USB CDC. W1 adds a non-blocking cooperative
scheduler and a line-delimited-JSON command console over that same USB CDC
link (see `../ARCHITECTURE.md` section 2 for the protocol). Still no LCD, SD
card, Wi-Fi or BLE.

## ⚠️ Uploading repartitions the device

```
pio run -t upload
```

will **erase and repartition** the dongle. The device currently holds LilyGO's
factory demo on the *original* 4 MB, single-slot partition table (see
`../backup/factory_release/README.md`) — that layout has no OTA slot, no
rollback, and once overwritten there is no way back except a full reflash from
the backup.

**Before uploading, confirm `../backup/factory_release/restore.sh` still restores
the device from a clean/blank state.** It was verified against this exact device
on 2026-08-15 while factory-fresh; that backup is the *only* fallback (see
`ARCHITECTURE.md` section 3, "Repartitioning is NOT a standalone step").

This build step is **build only**: `pio run` compiles and links but never opens
`/dev/ttyACM0` or touches the device. Flashing is a separate, explicitly
authorised step.

## Build

```bash
~/.local/bin/pio run -d firmware
```

First build downloads the pinned pioarduino toolchain (compiler, ESP-IDF
components, tools) — expect a large one-off download. Subsequent builds are
incremental.

Both envs must build clean: `-e t-dongle-s3-tinyusb` (default, TinyUSB) and
`-e t-dongle-s3` (USB-Serial/JTAG fallback).

## Host tests

```bash
~/.local/bin/pio test -d firmware -e native
```

Compiles `src/claims.h` + `src/claims_selftest.h` for this machine and asserts
the claim arbitration rule. No hardware, no serial port, no upload.

`-e native` is **required**. The device envs set `test_ignore = *` precisely so
that a bare `pio test` cannot decide to build the tests for the dongle and
upload them — which would repartition it.

## Layout

| Path | What |
|---|---|
| `platformio.ini` | Build config. Pins the pioarduino platform to an explicit release (not a floating branch), points at the custom board and partition table. |
| `boards/lilygo-t-dongle-s3.json` | Custom board definition — 16 MB flash, no PSRAM, QIO, 240 MHz, native USB CDC-on-boot. There is no official upstream PlatformIO board entry for this device; this is derived from LilyGO's own `boards/dongles3.json` in the [T-Dongle-S3 repo](https://github.com/Xinyuan-LilyGO/T-Dongle-S3). |
| `partitions.csv` | The 16 MB dual-OTA layout from `../ARCHITECTURE.md` section 3: `nvs` (32 K) / `nvs_keys` (4 K, `encrypted` flag, reserved for future NVS encryption) / `otadata` / a 48 K pad for 64 K app alignment / `app0` (ota_0, 4 MB) / `app1` (ota_1, 4 MB) / `littlefs` (7.75 MB) / `coredump` (128 K). Replaces the factory 4 MB single-slot layout. |
| `scripts/check_boot_app0.py` | Build-time (`pre:`) guard: fails the build if `boards/lilygo-t-dongle-s3.json`'s `upload.arduino.boot_app0` doesn't match the `otadata` offset in `partitions.csv`, so an edit to one that isn't mirrored in the other can't silently corrupt NVS at flash time. |
| `src/main.cpp` | `setup()`: prints the boot banner (chip info, live partition table, running OTA slot/state — unchanged from W0), calls `registry.begin()`, registers the modules, replays their persisted state, then registers the two non-module scheduler tasks. `loop()` just calls `scheduler.run()` — no `delay()` in the main path. |
| `src/scheduler.h/.cpp` | Small fixed-size cooperative task scheduler: register a name/interval/callback, `run()` once per `loop()` pass. Tracks last-run time, run count, last and worst-case execution time per task (see the `tasks` console command) and never lets an overrunning task starve the others by catching up in a burst. |
| `src/claims.h` | Resource enum (`usb`/`sd`/`wifi`/`ble`/`lcd`/`led`), shared/exclusive claim sets, and the whole arbitration rule. **Dependency-free** (`<stdint.h>` only) so it compiles for the host — see `[env:native]`. |
| `src/claims_selftest.h` | The arbitration assertion table. Run on-target by the `selftest` command and on the host by `pio test -e native`; one table, two runners. |
| `src/modparam.h` | The machine-readable action-parameter schema (`ParamType` / `ModuleParam`) a module's action table is built from, plus the enum-list splitter `Registry::list()` emits through. **Dependency-free**, so `pio test -e native` covers it. This is what lets the web UI render a real form per action instead of demanding hand-typed JSON. |
| `src/modset.h` | The persisted module-id list format (`"a,b,c"`) and the boot decision `ModSet::wantAtBoot()` — including how a module NEW to a device gets its `defaultEnabled` while one the owner turned off stays off. **Dependency-free**; read the header before touching NVS persistence. |
| `src/registry.h/.cpp` | The module registry — the contract W2/W3 compile against. Descriptors, claim arbitration, enable/disable with `force`, NVS persistence, per-module scheduler ticks, and a recursive mutex around the public API. Read the header comment before writing a module. |
| `src/bus.h/.cpp` | Transport-agnostic event bus. Modules call `Bus::emit()`; transports register a sink. A module must never call into a transport directly. |
| `src/protocol.h` | Wire framing constants shared by every transport — currently just the maximum line length, defined once so CDC/WS/BLE cannot disagree. |
| `src/mod_cdc.h/.cpp` | The `cdc` module: the serial console as a registry citizen, so its claim on USB is visible to arbitration. `essential` — cannot be disabled. |
| `src/mod_led.h/.cpp` | The `led` module: descriptor, actions, status and heartbeat tick for the APA102. The reference module — smallest thing that exercises the whole registry against real hardware. |
| `src/led.h/.cpp` | The onboard APA102 (GPIO 40 data / 39 clock) driver, moved out of `main.cpp`. Drives a slow amber heartbeat blink until the console's `led` command takes manual control. |
| `src/partition_info.h/.cpp` | The partition-subtype-name lookup shared by the boot banner and the `parts` console command, so there's one switch statement to keep in sync with the partition table. |
| `src/console.h/.cpp` | The USB CDC transport: line-delimited JSON per `../ARCHITECTURE.md` section 2, plus a bare-word shorthand (e.g. `info`, `led ff0000`) for typing by hand. Built-in commands: `help`, `info`, `parts`, `mem`, `uptime`, `tasks`, `led`, `modules`, `enable`, `disable`, `selftest`, `log`, `reboot`. |
| `test/test_claims/` | Host unit tests for the claim rule. `pio test -e native` — no device involved. |
| `test/test_modparam/`, `test/test_modset/` | Host unit tests for the action-parameter schema and for the boot decision (new module gets its default, a disabled one stays off, an unknown persisted id is tolerated). |
| `tools/console.py` | Host-side helper, run via `uv run --with pyserial python tools/console.py ...` — one-shot command/response, `--raw` literal JSON, `--monitor` for events, `--reset` to pulse DTR/RTS and capture the boot log. |

## Monitoring after a flash

```bash
~/.local/bin/pio device monitor -p /dev/ttyACM0 -b 115200
```

Only relevant once someone has explicitly chosen to flash — not part of the
build-only workflow above.
