"""Pre-upload: pulse the CDC port at 1200 baud to enter the ROM bootloader.

Only needed for the TinyUSB build (ARDUINO_USB_MODE=0).

Under ARDUINO_USB_MODE=1 the ESP32-S3's fixed-function USB-Serial/JTAG peripheral
handles esptool's DTR/RTS reset in hardware, so `pio run -t upload` just works.
Under TinyUSB that reset is implemented in *firmware*, and esptool's DTR/RTS
sequence does not trigger it -- uploads fail with:

    A fatal error occurred: Failed to connect to ESP32-S3: No serial data received.

Opening the port at 1200 baud is the Arduino "touch" convention; the TinyUSB CDC
stack watches for it and reboots into the ROM download mode. Measured on this
board 2026-08-16: 3/3 uploads clean with the touch, 3/3 failed without it. Costs
~2.5 s per flash (12.5 s vs 10.0 s for the hardware-reset path).

If this ever stops working the fallback is unchanged and always available: hold
BOOT (GPIO 0) while plugging in to force ROM download mode. The ROM loader is in
mask ROM and cannot be erased.
"""

import time

Import("env")  # noqa: F821  (injected by SCons)


def _touch_1200(source, target, env):
    try:
        import serial
    except ImportError:
        print("touch_reset: pyserial unavailable, skipping -- upload may fail to connect")
        return

    # $UPLOAD_PORT is usually still empty at pre-action time — PlatformIO resolves it
    # inside the upload action itself. Force the same auto-detection early so we know
    # which port to touch; without this the touch silently no-ops and the upload fails.
    port = env.subst("$UPLOAD_PORT")
    if not port:
        try:
            env.AutodetectUploadPort()
            port = env.subst("$UPLOAD_PORT")
        except Exception as exc:  # noqa: BLE001
            print(f"touch_reset: could not auto-detect upload port ({exc})")
            return
    if not port:
        print("touch_reset: no upload port found, skipping touch -- upload will likely fail")
        return

    try:
        serial.Serial(port, 1200).close()
    except Exception as exc:  # noqa: BLE001 - any serial failure is non-fatal here
        print(f"touch_reset: 1200-baud touch on {port} failed ({exc}); "
              "upload may fail -- hold BOOT while replugging if so")
        return

    print(f"touch_reset: pulsed {port} at 1200 baud, waiting for ROM bootloader")
    # The device drops off the bus and re-enumerates in download mode.
    time.sleep(1.5)


env.AddPreAction("upload", _touch_1200)  # noqa: F821
