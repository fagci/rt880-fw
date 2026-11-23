#!/usr/bin/env python3
"""
Замер скорости запуска через UART-консоль:
  reboot -> загрузчик -> приложение печатает READY/PROF.

Usage: python3 bootprof.py [--runs N] [--timeout SEC] <port>
"""

import argparse
import statistics
import sys
import time

import serial

STAGES = [
    ("hext", "hext"),
    ("pll", "pre"),
    ("uart", "uart"),
    ("gpio", "gpio"),
    ("adc", "adc"),
    ("lcd", "lcd"),
    ("clear", "clear"),
    ("board", "board"),
    ("lv", "lv"),
    ("sw", "sw"),
    ("upd", "upd"),
    ("draw", "draw"),
    ("ready", "ready"),
]


def read_line(port, deadline):
    line = bytearray()
    first = None
    while time.time() < deadline:
        b = port.read(1)
        if not b:
            continue
        if first is None:
            first = time.time()
        if b == b"\n":
            return bytes(line).rstrip(b"\r"), first
        line += b
    raise TimeoutError("timeout waiting for line")


def drain(port, quiet=0.05, timeout=1.0):
    """Дожидаемся тишины: после прошивки READY/PROF могут ещё идти."""
    end = time.time() + timeout
    last = time.time()
    while time.time() < end:
        if port.read(1):
            last = time.time()
        elif time.time() - last >= quiet:
            return


def run_once(port, timeout):
    drain(port)
    port.write(b"reboot\n")
    port.flush()

    ack_deadline = time.time() + 2.0
    while True:
        line, _ = read_line(port, ack_deadline)
        if line == b"REBOOT":
            break
        # Профиль предыдущей загрузки мог ещё Transmission'ся.
        if not (line.startswith(b"READY ") or line.startswith(b"PROF ")):
            raise RuntimeError(f"unexpected reboot response: {line!r}")

    # Reset происходит сразу после ACK; отсчёт — от его приёма.
    t0 = time.time()
    deadline = t0 + timeout
    ready_wall = None
    prof = {}

    while time.time() < deadline:
        line, first = read_line(port, deadline)
        if line.startswith(b"READY "):
            ready_wall = first - t0
        elif line.startswith(b"PROF "):
            for item in line.split()[1:]:
                key, _, value = item.partition(b"=")
                prof[key.decode()] = int(value)
            break

    if ready_wall is None:
        raise TimeoutError("no READY line")
    if not prof:
        raise TimeoutError("no PROF line")
    return ready_wall * 1000.0, prof


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    port = serial.Serial(args.port, 115200, timeout=0.05, write_timeout=1.0)
    rows = []
    try:
        for i in range(args.runs):
            try:
                wall, prof = run_once(port, args.timeout)
            except Exception as e:
                print(f"run {i + 1}: error: {e}")
                port.write(b"\n")
                time.sleep(0.2)
                continue
            rows.append((wall, prof))
            print(f"run {i + 1}: cold {wall:7.2f} ms, "
                  f"app {prof['ready'] / 1000.0:7.2f} ms, "
                  f"loader+pre {wall - prof['ready'] / 1000.0:7.2f} ms")
            time.sleep(0.15)
    finally:
        port.close()

    if not rows:
        print("No successful runs.", file=sys.stderr)
        sys.exit(1)

    print("\nstage           min       median    max       (ms, from main)")
    prev_key = None
    for label, key in STAGES:
        values = []
        for _, prof in rows:
            value = prof[key]
            if prev_key is not None:
                value -= prof[prev_key]
            values.append(value / 1000.0)
        prev_key = key
        print(f"{label:<10} {min(values):9.2f} {statistics.median(values):9.2f} "
              f"{max(values):9.2f}")

    cold = [wall for wall, _ in rows]
    app = [prof["ready"] / 1000.0 for _, prof in rows]
    print(f"\ncold boot: min {min(cold):.2f}  median "
          f"{statistics.median(cold):.2f}  max {max(cold):.2f} ms")
    print(f"app boot:  min {min(app):.2f}  median "
          f"{statistics.median(app):.2f}  max {max(app):.2f} ms")
    print(f"loader+pre: median {statistics.median(cold) - statistics.median(app):.2f} ms")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
