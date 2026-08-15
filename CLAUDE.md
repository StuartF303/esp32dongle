# usbdongle

Work with a **LilyGO T-Dongle-S3** (base model, not Plus/Dual) plugged into the front of
this machine.

## Verified hardware (esptool + flash dump + boot log, 2026-08-15)

| | |
|---|---|
| Chip | ESP32-S3 (QFN56), revision v0.2 |
| Cores | Dual core + LP core @ 240 MHz |
| Radios | Wi-Fi, Bluetooth 5 (LE) — Wi-Fi scan works, sees local APs |
| Crystal | 40 MHz |
| Flash | **16 MB** Winbond W25Q128 (`ef` / `4018`), quad I/O, 3.3 V |
| PSRAM | **None.** eFuse reports none, and LilyGO's own board definition is named "LilyGo T-Dongle S3 (16 MB QD, No PSRAM)". Settled. |
| MAC / USB serial | `e4:b3:23:f2:a9:d8` |
| USB | Native USB-Serial/JTAG, `303a:1001` → `/dev/ttyACM0` |

## Board pinout (from LilyGO factory_screen example)

LCD — ST7735, 160×80, SPI2_HOST, no MISO:

| MOSI | CLK | CS | DC | RST | Backlight |
|---|---|---|---|---|---|
| 3 | 5 | 4 | 2 | 1 | 38 (active **low**) |

microSD ("TF", inside the USB-A shell) — **SDMMC 4-bit**, not SPI:

| CLK | CMD | D0 | D1 | D2 | D3 |
|---|---|---|---|---|---|
| 12 | 16 | 14 | 17 | 21 | 18 |

Other: APA102 RGB LED on DI 40 / CI 39. Boot button GPIO 0. Broken-out header is just
3V3, GPIO43 (TX), GPIO44 (RX), GND — two usable GPIOs.

`T-Dongle-S3-schematic.pdf` (in this directory, from the LilyGO repo) confirms all of it:
one combined **USB-3.0-TYPE-A-TF** connector carries both USB D+/D− and the six SD lines,
the LCD is a 6-pin FPC, U3 is the W25Q flash, U5 the APA102. **No PSRAM part on the
board** — visual confirmation of the eFuse/board-JSON finding. There is **no card-detect
pin**, so firmware can't tell "no card" from "bad card" without trying to init it.
A schematic note reads "SD卡加上拉，厂家连接座线序反" — pull-ups added to the SD lines,
and the connector's pin order is reversed relative to the manufacturer's part.

IR LED, PDM microphone and the QWIIC I2C bus are **Plus-variant only** — this board has
none of them. (espboards.dev's page is wrong on this, and also wrongly claims 8 MB PSRAM,
a battery connector and an IPEX antenna. Trust the LilyGO repo, not that page.)

## Firmware currently on the device

LilyGO's own factory demo — do not assume it is disposable until stuart says so.

- Partition table (4 MB layout on a 16 MB chip): `nvs` 20K @ 0x9000, `otadata` 8K @ 0xe000,
  `app0` (ota_0) 3M @ 0x10000, `spiffs` 896K @ 0x310000, `coredump` 64K @ 0x3f0000.
  No app1 — no OTA slot.
- Built with PlatformIO + Arduino-ESP32 (ESP-IDF v4.4.5), 12 Jun 2023, on the machine of
  `C:/Users/lewis/...` (Lewis He, LilyGO). Uses TFT_eSPI + LVGL + SD_MMC + WiFi.
- Boot log over `/dev/ttyACM0` @ 115200, with the card working:
  ```
  Hello T-Dongle-S3
  SD_MMC Card Type: SD_MMCHC
  SD_MMC Card Size: 120580
  Total space: 120534MB / Used space: 0MB
  scan start / scan done / 7 networks found
  ```

## microSD — resolved

A **128 GB** card is fitted (117.7 GiB, reports 120534 MB free, type SDHC/SDXC), on the
4-bit SDMMC bus. It originally failed to mount, and the cause was **exFAT, not capacity**:

- The boot log said `mount_to_vfs failed (0xffffffff)`, *not* `sdmmc_card_init failed`.
  In IDF 4.4.5 `vfs_fat_sdmmc.c` card init runs first and has its own distinct message,
  so getting to `mount_to_vfs` — whose `0xffffffff` is the `ESP_FAIL` set when `f_mount`
  fails — proved the card was present and talking. Only FatFs rejected it.
- 128 GB is SDXC and ships exFAT-formatted; the dumped app contains **zero** `exfat`
  strings (exFAT is compiled out by default in Arduino-ESP32). FatFs mounts FAT32 to
  2 TB, so size was never the limit.
- Fixed 2026-08-15 by reformatting: MBR partition type set to `0x0c`, then
  `mkfs.vfat -F 32 -s 64` (32 KB clusters), label `TDONGLE`. The card was empty, so
  nothing was lost.

**Keep this card FAT32.** Re-formatting it exFAT on a PC will silently break it for the
dongle again. Note also there is no card-detect pin, so "no card" and "unreadable card"
are indistinguishable to firmware except by which init step fails.

Upstream source: <https://github.com/Xinyuan-LilyGO/T-Dongle-S3> — has examples
(`sd_card`, `usb_mass_storage`, `usb_hid_keyboard/mouse`, `lcd`, `led`, `fs_webserver`),
schematics under `schematic/`, and PlatformIO board JSONs under `boards/`.

## Serial port access

`/dev/ttyACM0` is group `dialout`; `stuart` is in that group and it now works from the
Bash tool. Do **not** retry with `sudo` from the Bash tool: that shell has no TTY, so
sudo cannot prompt. Ask stuart to run it in his own terminal and paste the output.

esptool is not installed system-wide. Invoke via uv:

```bash
uvx --from esptool esptool --port /dev/ttyACM0 flash-id
```

esptool v5 deprecated the `esptool.py` name and underscore subcommands — use `esptool`
with `flash-id` / `chip-id`, not `flash_id` / `chip_id`.

**Always pass `--no-stub`.** esptool's stub flasher dies with "Packet content transfer
stopped" on this board — and *reproducibly at specific offsets* (`0x4e000` is one), not
randomly, so retrying and chunking do not help. The ROM loader is ~3× slower
(≈60 KB/s vs ≈150 KB/s) but completely reliable: it read all 3 MB of app0 in one call.

To grab the boot log, reset via DTR/RTS and read the port (pyserial via
`uv run --with pyserial python ...`) — esptool holds the port exclusively, so do one or
the other.

## Factory backup

`backup/factory_release/` holds a complete, `verify-flash`-checked image of the firmware
the dongle shipped with (bootloader, partition table, otadata, app0, spiffs, plus nvs and
coredump), with `restore.sh` to put it all back and a README covering the flashing
gotchas. Captured 2026-08-15 while the device was still factory-fresh.

**This is the only fallback** — there is no OTA slot — so it must stay intact.

## Status

All three original open questions (PSRAM, LCD, microSD) are now settled — see above.


Everything to date is **read-only**. Nothing has been written to the device. Confirm with
stuart before flashing anything — the factory demo is the only firmware on board and
there is no OTA slot to fall back to.

No application code yet. Target project undecided as of 2026-08-15.
