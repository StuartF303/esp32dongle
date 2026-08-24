---
name: flash-dongle
description: Build, flash and debug the LilyGO T-Dongle-S3 over USB. Use for any deploy-to-device, boot-log, console-command, or "it won't flash / won't boot" task in this repo. Contains the known-good command sequence and every failure mode seen so far with its actual cause.
---

# Flashing and debugging the T-Dongle-S3

Fast path first. Diagnose from the tables further down — every failure listed has been hit for
real on this board, and the obvious explanation was wrong more than once.

## Fast path

```bash
cd ~/projects/esp32dongle/firmware
~/.local/bin/pio run -d .              # build (guard scripts run first)
~/.local/bin/pio run -d . -t upload    # flash
sleep 5                                # native USB re-enumerates after the reset
uv run --with pyserial python tools/console.py info
```

Deploy is ~12 s end to end. Do not add `sudo`; the Bash tool has no TTY and sudo cannot
prompt. `stuart` is already in `dialout`.

**Two build environments.** Default is `t-dongle-s3-tinyusb` (`ARDUINO_USB_MODE=0`) — TinyUSB
composite, required for `hid`/`msc`. `-e t-dongle-s3` is the `ARDUINO_USB_MODE=1` fallback: the
fixed-function USB-Serial/JTAG peripheral, ~2 s faster to flash and the **only** way to get
USB-JTAG. Both share one partition table; switching between them is just a reflash.

The TinyUSB env needs `scripts/touch_reset.py` (already wired in) to flash at all — see the
`No serial data received` row below.

`pio` is at `~/.local/bin/pio` (uv-managed). It is **not** on the Bash tool's `PATH` — use the
full path or the call fails with `command not found`.

## Talking to the running device

`tools/console.py` is the debug channel. Line-delimited JSON over native USB CDC.

```bash
cd ~/projects/esp32dongle/firmware
uv run --with pyserial python tools/console.py info      # chip, MAC, IDF, running partition
uv run --with pyserial python tools/console.py parts     # LIVE partition table off the device
uv run --with pyserial python tools/console.py tasks     # scheduler table + worst-case runtimes
uv run --with pyserial python tools/console.py mem       # heap
uv run --with pyserial python tools/console.py uptime
uv run --with pyserial python tools/console.py 'led ff0000'
uv run --with pyserial python tools/console.py reboot
uv run --with pyserial python tools/console.py --monitor # stream events
```

**`parts` is the ground truth for whether a repartition took.** Do not infer it from a
successful flash — read it back.

## Known-good outcomes

Flash succeeded:
```
Wrote 329312 bytes (189204 compressed) at 0x00020000 in 2.9 seconds
Verifying written data...
Hash of data verified.
Hard resetting via RTS pin...
[SUCCESS]
```
`at 0x00020000` is the tell that the **new** layout is in use. The old factory layout put the
app at `0x10000`.

Build guard passed (prints before compilation, every build):
```
check_boot_app0: OK -- otadata @ 0x12000 matches boot_app0 in board JSON
```

Device alive, correct layout (`parts`, 2026-08-15):
```
nvs        data nvs       0x00009000    32K  0x00011000
nvs_keys   data nvs_keys  0x00011000     4K  0x00012000
otadata    data ota       0x00012000     8K  0x00014000
app0       app  ota_0     0x00020000  4096K  0x00420000
app1       app  ota_1     0x00420000  4096K  0x00820000
littlefs   data littlefs  0x00820000  7936K  0x00fe0000
coredump   data coredump  0x00fe0000   128K  0x01000000
```

Healthy idle: `free_heap` ~345 KB, `largest_free_block` ~286 KB, `console.poll` worst-case
~6 ms, `led.heartbeat` worst-case ~230 µs.

Console responsiveness: successive one-shot commands return in **0.05–0.15 s**. If they take
~2 s each, `Serial.setTxTimeoutMs(0)` has been lost from `setup()` — see the TX-stall row below.

`info` reports `"ota_state": "UNDEFINED"` after a **USB** flash. That is correct, not a fault:
`PENDING_VERIFY` only ever appears for an image delivered by OTA. A USB flash records no state
in `otadata`, so rollback machinery is not engaged at all. Do not go hunting for
`PENDING_VERIFY` after `pio run -t upload`.

`info` on a correctly-flashed device reports `"flash_bytes": 16777216`. **If it reports
8388608 you are running the factory bootloader**, whose image header declares 8 MB — partitions
above `0x800000` will be rejected and `littlefs` will not work.

## Known-bad outcomes

| Symptom | Real cause | Fix |
|---|---|---|
| `Packet content transfer stopped`, reproducibly at a fixed offset (`0x4e000` seen) | esptool's **stub flasher** is broken on this board. Retrying and chunking do not help — it fails at the same offset every time | `upload_flags = --no-stub` in `platformio.ini` (already set). For hand-rolled esptool, always pass `--no-stub` |
| `pio: command not found` | `~/.local/bin` not on the Bash tool's PATH | Use `~/.local/bin/pio` |
| `ModuleNotFoundError: No module named 'platformio'` | `~/.platformio/penv` was built against a Python that no longer exists (3.10 → 3.12 upgrade) | Don't repair it. `uv tool install platformio --with pip` |
| `MissingPackageManifestError: Could not find one of 'package.json'` plus a `sudo apt install python3-dev libffi-dev libssl-dev` hint | **Both messages are misleading.** Real cause: `No module named pip` — PlatformIO shells out to pip to install esptool's deps, and a uv venv has none | `uv tool install --force platformio --with pip`, then `rm -rf ~/.platformio/packages/tool-esptoolpy` and reinstall it |
| Build aborts with a `check_boot_app0` mismatch | `partitions.csv` otadata offset and `boards/…json` `upload.arduino.boot_app0` have drifted | Make them agree. This guard exists because `pioarduino-build.py:230` falls back to a hardcoded `0xe000` with no CSV awareness — a mismatch silently writes the OTA selector into whatever partition sits at that offset, corrupting it |
| Flash OK, but boot log is empty | Native USB-Serial/JTAG **re-enumerates on reset**, so the host misses everything printed before it reattaches. Not a fault | Use the console. `info` + `parts` expose everything the banner prints |
| `parts` shows `app0 @ 0x10000` / a `spiffs` partition | The repartition did not take — factory table still present | Re-flash. Check the `upload` output actually said `at 0x00020000` |
| Serial read returns nothing while esptool is running | esptool holds the port exclusively | Do one or the other, never both |
| `restore.sh` exits 2 with "stdin is not a terminal" | Deliberate. It refuses to prompt into the void | Pass `--yes`. `echo y \| ./restore.sh` no longer works |
| `A fatal error occurred: Failed to connect to ESP32-S3: No serial data received.` on the TinyUSB env | Under `ARDUINO_USB_MODE=0` the DTR/RTS reset is implemented in **firmware**, not the fixed-function peripheral, so esptool's reset never fires. Measured 3/3 uploads fail without a touch, 3/3 clean with | `scripts/touch_reset.py` pulses the port at 1200 baud (Arduino touch convention) and is wired into the env. If it prints `skipping touch`, the port auto-detect failed — check `/dev/ttyACM0` exists. Last resort: hold BOOT while replugging |
| `error: no response within 3.0s` on the **first** command right after `-t upload` | Not a fault. The post-flash reset re-enumerates native USB, and the 3 s default is too tight while the host re-attaches | `sleep 5` after upload, or `--timeout 8` on the first call. It answers normally from then on |
| `{"ok":false,"e":{"code":"ELINE","msg":"line too long, discarded"}}` on the first command after a **failed** upload | Not a fault — the console recovering correctly. esptool's SLIP sync frames were left in the device's RX buffer and parsed as one over-long line | Ignore it and re-issue the command. If it repeats indefinitely, something is genuinely spamming the port |
| Device enumerates but console times out repeatedly, even with a long timeout and no heartbeat events on a passive listen | App wedged, or flashed a bad build | Hold **BOOT (GPIO 0)** while plugging in → ROM download mode, then reflash |
| Console commands each take ~2 s; LED heartbeat stutters | `Serial.setTxTimeoutMs(0)` missing from the END of `setup()`. `HWCDC::write` has a 256-byte TX ring and retries `xRingbufferSend` at the 100 ms default when the host is attached but not draining — normal while debugging. Tasks run in registration order, so a blocked write in `heartbeat.event` delays `console.poll` | Restore `Serial.setTxTimeoutMs(0)` at the end of `setup()`. It is deliberately **20 ms during the banner** and 0 from the scheduler onwards — see the block comment in `main.cpp` |
| `tools/console.py --reset` "works" but the device never rebooted: it exits 0 and prints heartbeats whose `uptime_ms` keeps **rising** | **DTR/RTS reset does not exist on the TinyUSB build.** Under `ARDUINO_USB_MODE=0` the reset is implemented in firmware (the same reason `touch_reset.py` is needed to flash), so toggling the lines does nothing. The rising-uptime heartbeat stream reads exactly like a boot log and is not one — this cost a session | Reboot in band: `console.py reboot`, **tolerate the `SerialException`** as the port drops, then reopen in a poll loop (`/dev/ttyACM*` — it may come back on a different number). `scratchpad/bootcap.py` in a working session does this |
| `info.build` reads the same date **before and after** a flash | Not a fault and not a stale image. That string is `esp_app_desc_t.date/time`, baked in by **arduino-lib-builder** when the framework was built, not by our compile. It will not move no matter what you flash | Never use `info.build` to prove which image is live. Use a **behavioural** field — a status key the new build adds, a value it changes — or the binary-identity method below |
| After a hand-rolled `esptool ... --after hard-reset`, the descriptor is still `303a:1001` but the console is dead and nothing answers | **The board is sitting in ROM download mode.** Reproduced twice. `--after hard-reset` drives RTS, and under `ARDUINO_USB_MODE=0` the RTS reset path is firmware-side — unavailable precisely when the firmware is not running, which is the state esptool just left it in | Pass `--after watchdog-reset` instead; it recovers immediately. `pio run -t upload` is unaffected (it has `touch_reset.py` in front of it) |

## Proving which image is actually on the device

`info.build` cannot do it (see the table above). Two methods that can:

**Behavioural, cheap, and usually enough.** Read a field the new build adds or changes —
`display status`'s `qr_version`, a new status key, a changed help string. One console call.

**Binary identity, when it has to be certain.** Read `app0` back and diff it against the local
build:

```bash
uvx --from esptool esptool --port /dev/ttyACM0 --no-stub \
    read-flash 0x20000 0x152000 /tmp/app0.bin       # ~90 s at ROM-loader speed
cmp -l /tmp/app0.bin firmware/.pio/build/t-dongle-s3-tinyusb/firmware.bin | wc -l
```

Expect **near-identical, not identical**. A real match looks like 1,351,147 of 1,351,216 bytes
the same, with the differences confined to three places: the app-elf SHA-256 in the image
header, `__TIME__` inside the framework's own banner string, and the trailing image checksum.
Anything beyond those three regions means it is a different build.

Remember esptool holds the port exclusively — no console during the read — and **always
`--no-stub`**.

## Recovery

The ESP32-S3 ROM loader lives in mask ROM and **cannot be erased**, so this board is not
brickable. Hold BOOT (GPIO 0) while plugging in to force download mode.

To return to factory firmware:
```bash
cd ~/projects/esp32dongle/backup/factory_release
./restore.sh --yes
```
**Proven twice.** First on 2026-08-15 while the device was still factory-fresh, then again as a
full round-trip *from the repartitioned 16 MB layout* — restore → factory demo boots
(`Hello T-Dongle-S3`, SD mounted, Wi-Fi scan) → reflash W1 → console responds. That second run
is the one that matters: it is the only proof `restore.sh` can overwrite our 16 MB-header
bootloader and 7-entry table with the factory 8 MB-header, 5-entry ones.

It restores the **factory** bootloader, whose header declares 8 MB — correct for the factory
layout. Leftover data above `0x400000` from our layout is simply not referenced by the factory
partition table and is inert. Restoring takes ~50 s (3 MB app at ROM-loader speed); reflashing
W1 afterwards takes ~10 s.

## Gotchas that cost time before

- `pio run -t upload` writes bootloader + partition table + app together. The repartition is a
  side effect of a normal flash — there is no separate repartition step, and attempting one
  standalone under the factory 8 MB-header bootloader would not boot.
- Verify what a config key actually does before trusting it. `upload.ota_partition_offset`
  looks like the right knob and is **dead config** here — `espidf.py` never runs with
  `framework = arduino` alone. `upload.arduino.boot_app0` is the live one. Prove it by removing
  the key and observing the difference, not by reading the platform source.
- `__DATE__` in `info.build` reflects the toolchain's compile date, not necessarily today.
