# usbdongle — pre: extra_script, wired via `extra_scripts` in platformio.ini.
#
# Guards against boot_app0 landing in the wrong partition.
#
# pioarduino-build.py:230 does:
#     board_config.get("upload.arduino.boot_app0", "0xe000")
# — it resolves boot_app0 purely from the *board JSON*, with a hardcoded
# fallback, and has no awareness of partitions.csv at all. If someone edits
# partitions.csv (moves or resizes the otadata partition) and forgets to
# update upload.arduino.boot_app0 in boards/lilygo-t-dongle-s3.json to match,
# the bootloader will still seed NVS with the *old* boot_app0 offset. After
# the edit that offset may no longer be inside the otadata partition at all —
# it could land in nvs, in app0, or anywhere else — and get silently
# corrupted. This script fails the build loudly instead, before that can
# happen.
import csv
import json
import os
import sys

Import("env")  # noqa: F821 -- injected by PlatformIO's SCons runner


def fail(msg):
    sys.stderr.write("\n" + "!" * 78 + "\n")
    sys.stderr.write("check_boot_app0: boot_app0 / partitions.csv mismatch -- build stopped\n")
    sys.stderr.write(msg + "\n")
    sys.stderr.write(
        "\nFix: set upload.arduino.boot_app0 in boards/lilygo-t-dongle-s3.json to\n"
        "the offset of the data/ota (otadata) row in partitions.csv, or fix\n"
        "partitions.csv if the offset that moved there was the mistake.\n"
    )
    sys.stderr.write("!" * 78 + "\n\n")
    env.Exit(1)  # noqa: F821


def find_otadata_offset(csv_path):
    if not os.path.isfile(csv_path):
        fail("partitions.csv not found at %s" % csv_path)
    with open(csv_path, newline="") as f:
        for row in csv.reader(f):
            row = [c.strip() for c in row]
            if not row or not row[0] or row[0].startswith("#"):
                continue
            if len(row) < 4:
                continue
            name, ptype, subtype, offset = row[0], row[1], row[2], row[3]
            if ptype == "data" and subtype == "ota":
                return name, int(offset, 0)
    fail("no data/ota row found in %s -- expected an otadata partition" % csv_path)


def find_boot_app0(board_json_path):
    if not os.path.isfile(board_json_path):
        fail("board definition not found at %s" % board_json_path)
    with open(board_json_path) as f:
        board = json.load(f)
    try:
        raw = board["upload"]["arduino"]["boot_app0"]
    except KeyError:
        fail(
            "%s has no upload.arduino.boot_app0 -- pioarduino-build.py:230 "
            "would silently fall back to 0xe000" % board_json_path
        )
    return int(raw, 0)


project_dir = env["PROJECT_DIR"]  # noqa: F821
csv_path = os.path.join(project_dir, "partitions.csv")
board_json_path = os.path.join(project_dir, "boards", "lilygo-t-dongle-s3.json")

otadata_name, otadata_offset = find_otadata_offset(csv_path)
boot_app0_offset = find_boot_app0(board_json_path)

if otadata_offset != boot_app0_offset:
    fail(
        "%s: '%s' (data/ota) is at 0x%x, but %s has "
        "upload.arduino.boot_app0 = 0x%x. The bootloader's boot_app0 NVS blob "
        "is what selects the OTA slot to boot; if it lands outside the "
        "actual otadata partition it will corrupt whatever partition IS at "
        "that offset instead."
        % (csv_path, otadata_name, otadata_offset, board_json_path, boot_app0_offset)
    )

print("check_boot_app0: OK -- otadata @ 0x%x matches boot_app0 in board JSON" % otadata_offset)
