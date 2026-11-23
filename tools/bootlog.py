#!/usr/bin/env python3
"""Auto-reconnecting serial logger for the AI Passport device (host side).

Runs next to the device on macOS/Linux; needs only pyserial (already pulled
in by esptool). Keeps scanning the port pattern and reattaches after every
deep-sleep USB re-enumeration, so logging survives device reboots and the
port name changing. Raw output goes to the log file and is echoed to the
terminal; attach/detach timestamps mark each connection span in the file.

Stop this tool before flashing: a concurrent serial reader eats esptool's
SLIP responses and the write aborts mid-image with "chip stopped responding".

Usage:
  python3 tools/bootlog.py                      # default: /dev/cu.usbmodem* -> boot.log
  python3 tools/bootlog.py -p /dev/ttyUSB0      # Linux or an exact port
  python3 tools/bootlog.py -p '/dev/cu.usbmodem*' -o run.log
Stop: Ctrl+C (foreground) or: pkill -f tools/bootlog.py
"""
import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip3 install pyserial")


def stamp(banner: str, log) -> None:
    line = f"===== {time.strftime('%Y-%m-%d %H:%M:%S')} {banner} =====\n"
    log.write(line.encode())
    log.flush()
    print(line, end="", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Auto-reconnecting serial logger for the AI Passport device.")
    ap.add_argument("-p", "--port", default="/dev/cu.usbmodem*",
                    help="serial port or glob pattern (default: %(default)s)")
    ap.add_argument("-o", "--out", default="boot.log",
                    help="output log file (default: %(default)s)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    args = ap.parse_args()

    with open(args.out, "ab") as log:
        while True:
            port = next(iter(sorted(glob.glob(args.port))), None)
            if port is None:
                time.sleep(0.2)
                continue
            stamp(f"attach {port}", log)
            try:
                ser = serial.Serial(port, args.baud, timeout=0.5)
                while True:
                    data = ser.read(4096)
                    if data:
                        log.write(data)
                        log.flush()
                        print(data.decode("utf-8", "replace"), end="")
            except KeyboardInterrupt:
                raise
            except Exception:
                pass  # device detached (deep sleep / reboot): rescan
            finally:
                stamp("detach", log)
            time.sleep(0.2)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nbootlog: stopped")
        sys.exit(0)
