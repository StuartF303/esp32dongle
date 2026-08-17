# Stock LilyGO firmware — attribution and provenance

**These binaries are not ours.** `bootloader.bin`, `partition-table.bin`, `app0.bin`, `spiffs.bin`,
`otadata.bin`, `nvs.bin` and `coredump.bin` are a byte-for-byte capture of the firmware that
**LilyGO** ships pre-installed on the T-Dongle-S3. The application is their `factory_screen`
example, built by Lewis He on 12 Jun 2023.

- Upstream source and the original firmware: <https://github.com/Xinyuan-LilyGO/T-Dongle-S3>
- Copyright and licence: LilyGO / Xinyuan. Refer to that repository for terms.
- Board vendor: <https://lilygo.cc>

### Why it is in this repository

The T-Dongle-S3 ships with **no OTA slot** — a single `app0` partition and nothing to fall back
on. Reflashing it destroys the stock firmware permanently, and LilyGO does not publish a prebuilt
binary image of it (only source, which will not rebuild bit-identically). This capture was taken
from the device on 2026-08-15 **before anything was written to it**, and every region was verified
against the hardware with `esptool verify-flash`.

It is kept here so the device can always be returned to stock, and so anyone else who has already
reflashed theirs can recover. `restore.sh` puts it all back; the round trip has been tested from a
fully repartitioned device.

If you are LilyGO and would prefer this not be redistributed, open an issue and it will be removed.

---

# T-Dongle-S3 factory firmware backup

A complete, verified image of the firmware this **LilyGO T-Dongle-S3** shipped with,
read off the device on **2026-08-15** before anything was ever written to it.

To put the dongle back exactly as it came:

```bash
./restore.sh                 # prompts before writing
./restore.sh --yes           # no prompt — for scripts and agents (-y also works)
./restore.sh --with-nvs      # also restore the factory NVS contents
./restore.sh --port /dev/X   # a different serial port
```

`--yes` exists so the script can run unattended. It is never implied: without it, and with
stdin not a terminal, the script **refuses to run** (exit 2) rather than prompting into the
void. It still prints everything it is about to write before it writes it, either way.

## What the device was running

LilyGO's own factory demo, matching `examples/factory_screen` in
<https://github.com/Xinyuan-LilyGO/T-Dongle-S3>.

| | |
|---|---|
| Build | PlatformIO + Arduino-ESP32, ESP-IDF v4.4.5 |
| Built | 12 Jun 2023, 16:32:29 |
| App ELF SHA-256 | `10f8881f18d795b13a90247dba1bd75bd4e3c9e918485c76c276769ed5f9af95` |
| Libraries | TFT_eSPI, LVGL, SD_MMC, WiFi |
| Behaviour | ST7735 LCD demo, mounts the microSD at `/sdcard`, scans Wi-Fi |

Device it came from: ESP32-S3 rev v0.2, 16 MB flash, MAC `e4:b3:23:f2:a9:d8`
(see `device-info.txt`). `boot-log.txt` is what a healthy boot looks like — use it to
confirm a restore worked.

## Files

| File | Flash offset | Size | Restored? |
|---|---|---|---|
| `bootloader.bin` | `0x0` | 32 K | yes |
| `partition-table.bin` | `0x8000` | 4 K | yes |
| `nvs.bin` | `0x9000` | 20 K | only with `--with-nvs` |
| `otadata.bin` | `0xe000` | 8 K | yes |
| `app0.bin` | `0x10000` | 3 M | yes |
| `spiffs.bin` | `0x310000` | 896 K | yes |
| `coredump.bin` | `0x3f0000` | 64 K | no — diagnostic only |

That covers every region the partition table describes. Flash above `0x400000` is
unused erased space on this 16 MB chip and is not backed up.

`nvs.bin` is off by default because NVS is runtime state (Wi-Fi credentials, app
settings), not firmware — a fresh NVS is usually what you want. Pass `--with-nvs` to put
the factory contents back too.

## Integrity

Every image was read back and checked against the device with
`esptool verify-flash`; all seven reported *"Verification successful (digest matched)"*.
`SHA256SUMS` covers the files as stored here, and `restore.sh` checks it before writing.

## Gotchas worth knowing before you flash

- **Use `--no-stub`.** esptool's stub flasher intermittently fails on this board with
  `Packet content transfer stopped`, and it does so *reproducibly at certain offsets*
  (`0x4e000` was one). The ROM loader is ~3× slower but reliable. `restore.sh` already
  passes `--no-stub`; remember it for any hand-rolled esptool commands too.
- **There is no OTA slot.** The partition table has `app0` only, no `app1`, so a bad
  flash leaves nothing to fall back on. This backup *is* the fallback.
- **`--flash-mode/freq/size keep`** preserves the dumped image header. Don't let esptool
  rewrite it — the factory header declares an 8 MB flash size even though the chip is
  16 MB, and that is how it shipped.
- **Recovery if a flash goes wrong:** hold the BOOT button (GPIO 0) while plugging the
  dongle in to force the ROM download mode, then run `restore.sh`. The S3's ROM loader is
  in mask ROM and cannot be erased, so a bad flash is recoverable no matter what state the
  SPI flash is left in.
- **Piping an answer no longer works.** `echo y | ./restore.sh` used to be consumed by the
  `read` prompt and proceed; it now refuses (exit 2). Pass `--yes` explicitly instead.
- **Keep the microSD card FAT32.** The factory app has no exFAT support, so reformatting
  the card exFAT on a PC will make it unmountable on the dongle again.
