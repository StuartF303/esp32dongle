#!/usr/bin/env bash
#
# Restore the LilyGO T-Dongle-S3 to the factory firmware captured on 2026-08-15.
#
#   ./restore.sh                 # restore bootloader + partition table + app + spiffs
#   ./restore.sh --with-nvs      # also restore the factory NVS contents
#   ./restore.sh --port /dev/X   # use a different serial port
#
# Every image in this directory was read from the device and verified byte-for-byte
# against it with `esptool verify-flash` before being saved.
#
set -euo pipefail

PORT=/dev/ttyACM0
WITH_NVS=0

while (($#)); do
    case $1 in
        --port)     PORT=$2; shift 2 ;;
        --with-nvs) WITH_NVS=1; shift ;;
        -h|--help)  sed -n '2,12p' "$0"; exit 0 ;;
        *)          echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "$(dirname "$0")"

echo "Checking image integrity..."
sha256sum -c SHA256SUMS

# --no-stub matters: the stub flasher intermittently dies with
# "Packet content transfer stopped" on this board. The ROM loader is slower but reliable.
ESP=(uvx --from esptool esptool --port "$PORT" --no-stub)

# --flash-mode/freq/size "keep" preserves the image header exactly as it was dumped.
ARGS=(write-flash --flash-mode keep --flash-freq keep --flash-size keep)

FILES=(
    0x0      bootloader.bin
    0x8000   partition-table.bin
    0xe000   otadata.bin
    0x10000  app0.bin
    0x310000 spiffs.bin
)

if ((WITH_NVS)); then
    FILES+=(0x9000 nvs.bin)
fi

echo
echo "About to WRITE to $PORT:"
for ((i = 0; i < ${#FILES[@]}; i += 2)); do
    printf '  %-10s %s\n' "${FILES[i]}" "${FILES[i+1]}"
done
read -rp "Proceed? [y/N] " reply
[[ $reply == [yY] ]] || { echo "aborted"; exit 1; }

"${ESP[@]}" "${ARGS[@]}" "${FILES[@]}"

echo
echo "Verifying..."
for ((i = 0; i < ${#FILES[@]}; i += 2)); do
    "${ESP[@]}" --after no-reset verify-flash "${FILES[i]}" "${FILES[i+1]}"
done

echo
echo "Restored. Watch the boot log with:"
echo "  uv run --with pyserial python - <<'EOF'"
echo "  import serial,time; p=serial.Serial('$PORT',115200,timeout=0.2)"
echo "  p.setRTS(True); time.sleep(0.1); p.setRTS(False)"
echo "  t=time.time()+10"
echo "  while time.time()<t: print(p.read(4096).decode('utf-8','replace'),end='')"
echo "  EOF"
echo "Expect: 'Hello T-Dongle-S3' followed by SD card info and a Wi-Fi scan."
