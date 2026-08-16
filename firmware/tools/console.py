#!/usr/bin/env python3
"""Host-side helper for the usbdongle command console (see ../../ARCHITECTURE.md sec 2).

Run via uv, no system installs needed:

    uv run --with pyserial python tools/console.py info
    uv run --with pyserial python tools/console.py led ff0000
    uv run --with pyserial python tools/console.py --raw '{"id":1,"act":"info"}'
    uv run --with pyserial python tools/console.py --monitor
    uv run --with pyserial python tools/console.py --reset

One-shot mode (`console.py <command> [args...]` or `--raw ...`) sends one
line, waits up to --timeout seconds for the matching response line, prints it
as JSON, and exits 0 on {"ok":true}, 1 on {"ok":false}, or 2 on timeout /
usage error. It never hangs past --timeout.
"""

import argparse
import json
import sys
import time

import serial


def open_port(port, timeout):
    return serial.Serial(port, baudrate=115200, timeout=timeout)


def one_shot(ser, line, timeout):
    """Send `line`, return the first parsed {"ok":...} response dict, or None on timeout.

    Event lines ({"ev":...}) and any non-JSON noise (e.g. a boot banner) are
    skipped rather than treated as the response.
    """
    ser.reset_input_buffer()
    ser.write((line + "\n").encode("utf-8"))
    ser.flush()

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = ser.readline()  # bounded by the port's own read timeout
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace").strip()
        if not text:
            continue
        try:
            msg = json.loads(text)
        except json.JSONDecodeError:
            continue
        if not isinstance(msg, dict) or "ok" not in msg:
            continue  # an event, or something else entirely -- keep waiting
        return msg
    return None


def monitor(ser):
    print("-- monitoring, Ctrl-C to stop --", file=sys.stderr)
    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip()
            if text:
                print(text)
    except KeyboardInterrupt:
        pass


def do_reset(port, timeout):
    ser = open_port(port, timeout=0.2)

    # Native USB-Serial/JTAG: a plain RTS toggle is unreliable on this board.
    # This is the sequence that's proven to work -- see ../../CLAUDE.md.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    ser.close()

    # Native USB re-enumerates on reset, so the port can briefly disappear.
    reopened = None
    reopen_deadline = time.monotonic() + 5.0
    while time.monotonic() < reopen_deadline:
        try:
            reopened = serial.Serial(port, baudrate=115200, timeout=0.2)
            break
        except serial.SerialException:
            time.sleep(0.1)
    if reopened is None:
        print("error: %s did not reappear after reset" % port, file=sys.stderr)
        return 2

    deadline = time.monotonic() + timeout
    with reopened:
        while time.monotonic() < deadline:
            raw = reopened.readline()
            if raw:
                text = raw.decode("utf-8", errors="replace").rstrip()
                if text:
                    print(text)
    return 0


def main():
    ap = argparse.ArgumentParser(description="usbdongle command console")
    ap.add_argument("--port", default="/dev/ttyACM0", help="serial device (default /dev/ttyACM0)")
    ap.add_argument("--timeout", type=float, default=3.0, help="seconds to wait for a response (default 3)")
    ap.add_argument("--raw", help="send this literal JSON line instead of building one from the command args")
    ap.add_argument("--monitor", action="store_true", help="stream lines until interrupted (Ctrl-C)")
    ap.add_argument("--reset", action="store_true", help="pulse DTR/RTS, reopen the port, and print the boot log")
    ap.add_argument("command", nargs="*", help="bare-word command and args, e.g. 'info' or 'led ff0000'")
    args = ap.parse_args()

    if args.reset:
        return do_reset(args.port, max(args.timeout, 3.0))

    if args.monitor:
        ser = open_port(args.port, timeout=0.5)
        monitor(ser)
        return 0

    if args.raw:
        line = args.raw
    elif args.command:
        line = " ".join(args.command)
    else:
        ap.error("give a command (e.g. 'info'), or use --raw / --monitor / --reset")
        return 2  # unreachable -- ap.error() calls sys.exit()

    ser = open_port(args.port, timeout=0.2)
    resp = one_shot(ser, line, args.timeout)
    if resp is None:
        print("error: no response within %.1fs" % args.timeout, file=sys.stderr)
        return 2

    print(json.dumps(resp))
    return 0 if resp.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
