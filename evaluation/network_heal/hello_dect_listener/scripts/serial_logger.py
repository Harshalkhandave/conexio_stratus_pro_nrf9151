#!/usr/bin/env python3
"""
Capture DECT listener (or any board) serial logs:
  - print live to the terminal
  - save everything to a .txt file

Examples (Windows):
  python serial_logger.py COM5
  python serial_logger.py COM5 --baud 115200
  python serial_logger.py COM5 --out logs/run1.txt
  python serial_logger.py --list
"""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Missing dependency: pyserial")
    print("Install with:  python -m pip install -r requirements.txt")
    sys.exit(1)


DEFAULT_BAUD = 115200


def list_serial_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    print("Available serial ports:")
    for p in ports:
        print(f"  {p.device:10}  {p.description}  [{p.hwid}]")


def default_log_path() -> Path:
    logs_dir = Path(__file__).resolve().parent / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return logs_dir / f"listener_{stamp}.txt"


def open_serial(port: str, baud: int, retries: int = 20) -> serial.Serial:
    last_err: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            ser = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.2,
            )
            # Give USB-CDC a moment after open
            time.sleep(0.3)
            ser.reset_input_buffer()
            return ser
        except serial.SerialException as err:
            last_err = err
            print(f"[{attempt}/{retries}] Waiting for {port}: {err}")
            time.sleep(1.0)
    raise SystemExit(f"Could not open {port}: {last_err}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Mirror serial output to terminal and save to a .txt file."
    )
    parser.add_argument(
        "port",
        nargs="?",
        help="Serial port, e.g. COM5 (Windows) or /dev/ttyACM0 (Linux)",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=DEFAULT_BAUD,
        help=f"Baud rate (default: {DEFAULT_BAUD})",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output .txt path (default: scripts/logs/listener_YYYYMMDD_HHMMSS.txt)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List serial ports and exit",
    )
    parser.add_argument(
        "--no-timestamp",
        action="store_true",
        help="Do not prefix each line with a host timestamp",
    )
    args = parser.parse_args()

    if args.list or not args.port:
        list_serial_ports()
        if not args.port:
            print("\nUsage: python serial_logger.py COM5")
            return 0 if args.list else 1

    out_path = args.out if args.out is not None else default_log_path()
    out_path = out_path.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"Port     : {args.port}")
    print(f"Baud     : {args.baud}")
    print(f"Log file : {out_path}")
    print("Ctrl+C to stop.\n")

    ser = open_serial(args.port, args.baud)
    buffer = bytearray()

    try:
        with out_path.open("a", encoding="utf-8", errors="replace", newline="") as logf:
            header = (
                f"===== serial capture start "
                f"{datetime.now().isoformat(timespec='seconds')} "
                f"port={args.port} baud={args.baud} =====\n"
            )
            logf.write(header)
            logf.flush()
            print(header, end="")

            while True:
                chunk = ser.read(256)
                if not chunk:
                    continue

                buffer.extend(chunk)

                while True:
                    nl = buffer.find(b"\n")
                    if nl < 0:
                        break
                    raw = bytes(buffer[: nl + 1])
                    del buffer[: nl + 1]

                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    if args.no_timestamp:
                        text = line
                    else:
                        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        text = f"[{ts}] {line}"

                    print(text, flush=True)
                    logf.write(text + "\n")
                    logf.flush()

    except KeyboardInterrupt:
        print("\nStopped by user.")
    finally:
        if ser.is_open:
            ser.close()
        print(f"Saved: {out_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
