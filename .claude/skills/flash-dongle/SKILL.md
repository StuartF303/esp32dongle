---
name: flash-dongle
description: Build, flash and debug the LilyGO T-Dongle-S3 over USB. Use for any deploy-to-device, boot-log, console-command, or "it won't flash / won't boot" task in this repo. Contains the known-good command sequence and every failure mode seen so far with its actual cause.
---

# Flashing and debugging the T-Dongle-S3

Fast path first. Diagnose from the tables further down — every failure listed has been hit for
real on this board, and the obvious explanation was wrong more than once.

## Fast path

```bash
cd ~/projects/usbdongle/firmware
~/.local/bin/pio run -d .              # build (guard script runs first)
~/.local/bin/pio run -d . -t upload    # flash — REPARTITIONS, destroys factory demo
uv run --with pyserial python tools/console.py info
```

Deploy takes ~10 s end to end. Do not add `sudo`; the Bash tool has no TTY and sudo cannot
prompt. `stuart` is already in `dialout`.

`pio` is at `~/.local/bin/pio` (uv-managed). It is **not** on the Bash tool's `PATH` — use the
full path or the call fails with `command not found`.

## Talking to the running device

`tools/console.py` is the debug channel. Line-delimited JSON over native USB CDC.

```bash
cd ~/projects/usbdongle/firmware
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
| `error: no response within 3.0s` on the **first** command right after `-t upload` | Not a fault. The post-flash reset re-enumerates native USB, and the 3 s default is too tight while the host re-attaches | `sleep 5` after upload, or `--timeout 8` on the first call. It answers normally from then on |
| Device enumerates but console times out repeatedly, even with a long timeout and no heartbeat events on a passive listen | App wedged, or flashed a bad build | Hold **BOOT (GPIO 0)** while plugging in → ROM download mode, then reflash |
| Console commands each take ~2 s; LED heartbeat stutters | `Serial.setTxTimeoutMs(0)` missing from `setup()`. `HWCDC::write` has a 256-byte TX ring and retries `xRingbufferSend` 20× at a 100 ms default when the host is attached but not draining — normal while debugging. Tasks run in registration order, so a blocked write in `heartbeat.event` delays `console.poll` | Restore `Serial.setTxTimeoutMs(0)` immediately after `Serial.begin()` |

## Recovery

The ESP32-S3 ROM loader lives in mask ROM and **cannot be erased**, so this board is not
brickable. Hold BOOT (GPIO 0) while plugging in to force download mode.

To return to factory firmware:
```bash
cd ~/projects/usbdongle/backup/factory_release
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
