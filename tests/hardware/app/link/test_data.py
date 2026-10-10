#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end data-port throughput test for two connected ZLRS devices.
#
# Usage:
#   python3 tests/hardware/app/link/test_data.py --tx-port <TX serial> --rx-port <RX serial>
#   python3 tests/hardware/app/link/test_data.py --tx-port /dev/ttyACM0 --rx-port /dev/ttyACM2
#
# Serial fragments are resolved against /dev/serial/by-id/*-if00. The test sends distinct
# deterministic payloads in both directions, checks every received byte and reports JSON
# throughput. It needs pyserial and devices already running a paired data-link firmware.

import argparse
import errno
import glob
import json
import random
import sys
import threading
import time
from pathlib import Path

BY_ID_DIR = "/dev/serial/by-id"
DATA_INTERFACE = "-if00"
DEFAULT_BAUD = 115200


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def nonnegative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be zero or greater")
    return parsed


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def format_bytes(count: int) -> str:
    if count >= 1_000_000:
        return f"{count / 1_000_000:.2f} MBytes"
    if count >= 1_000:
        return f"{count / 1_000:.2f} KBytes"
    return f"{count} Bytes"


def format_rate(byte_count: int, duration_s: float) -> str:
    bits_per_second = byte_count * 8 / max(duration_s, 1e-9)
    if bits_per_second >= 1_000_000:
        return f"{bits_per_second / 1_000_000:.2f} Mbits/sec"
    if bits_per_second >= 1_000:
        return f"{bits_per_second / 1_000:.2f} Kbits/sec"
    return f"{bits_per_second:.2f} bits/sec"


def resolve_port(value: str) -> str:
    """Resolve a serial-number fragment to its data CDC; accept explicit OS port paths too."""
    if "/" in value or "\\" in value:
        return value

    matches = [path for path in glob.glob(f"{BY_ID_DIR}/*")
               if DATA_INTERFACE in Path(path).name and value.lower() in Path(path).name.lower()]
    if len(matches) > 1:
        raise ValueError(f"{value!r} matches several data ports: {', '.join(matches)}")
    # Keep unmatched values as OS serial names, such as COM4 on Windows.
    return matches[0] if matches else value


def read_port(index: int, port, received: list[bytearray], stop: threading.Event,
              errors: list[str]) -> None:
    try:
        while not stop.is_set():
            data = port.read(4096)
            if data:
                received[index].extend(data)
    except Exception as exc:  # report reader failures in the result
        errors.append(f"reader {index}: {exc}")


def drain_stale_input(ports, quiet_s: float = 1.0,
                      timeout_s: float = 10.0) -> tuple[list[int], bool]:
    """Discard and count bytes already queued at the host before measuring a new payload."""
    drained = [0, 0]
    started = time.monotonic()
    last_data = started
    while time.monotonic() - started < timeout_s:
        got_data = False
        for index, port in enumerate(ports):
            data = port.read(4096)
            if data:
                drained[index] += len(data)
                got_data = True
        now = time.monotonic()
        if got_data:
            last_data = now
        elif now - last_data >= quiet_s:
            for port in ports:
                port.reset_input_buffer()
            return drained, True
    return drained, False


def run_round(ports, round_no: int, count: int, chunk_size: int, interval_ms: int,
              timeout_s: float, settle_ms: int, report_interval_s: float,
              show_progress: bool) -> dict:
    drained, input_quiet = drain_stale_input(ports)
    if show_progress:
        print(f"  pre-test drain: TX input {format_bytes(drained[0])}, "
              f"RX input {format_bytes(drained[1])}; "
              f"{'quiet' if input_quiet else 'still active after 10 sec'}", flush=True)

    expected = [random.Random(6000 + round_no * 2 + i).randbytes(count) for i in range(2)]
    received = [bytearray(), bytearray()]
    errors: list[str] = []
    if not input_quiet:
        errors.append("data ports did not become quiet before the benchmark")
    stop = threading.Event()
    readers = [threading.Thread(target=read_port, args=(i, ports[i], received, stop, errors))
               for i in range(2)]
    for reader in readers:
        reader.start()

    start = time.monotonic()
    deadline = start + timeout_s
    direction_completed_at = [None, None]
    last_report_at = start
    last_report_counts = [0, 0]

    def report_progress(force: bool = False) -> None:
        nonlocal last_report_at, last_report_counts
        now = time.monotonic()
        elapsed = now - last_report_at
        if not show_progress or (not force and elapsed < report_interval_s):
            return

        counts = [len(data) for data in received]
        for name, receive_index in (("TX->RX", 1), ("RX->TX", 0)):
            delta = max(counts[receive_index] - last_report_counts[receive_index], 0)
            print(f"[{name:5}] {last_report_at - start:6.2f}-{now - start:6.2f} sec  "
                  f"{format_bytes(delta):>12}  {format_rate(delta, elapsed)}", flush=True)
        last_report_at = now
        last_report_counts = counts

    try:
        for offset in range(0, count, chunk_size):
            if time.monotonic() >= deadline:
                raise TimeoutError("round timed out while sending")
            chunk_end = min(offset + chunk_size, count)
            for i, port in enumerate(ports):
                payload = expected[i][offset:chunk_end]
                endpoint = "TX" if i == 0 else "RX"
                try:
                    written = port.write(payload)
                except Exception as exc:
                    raise TimeoutError(
                        f"write to {endpoint} data port {port.port} failed: {exc}") from exc
                if written != len(payload):
                    raise TimeoutError(
                        f"short write to {endpoint} data port {port.port}: "
                        f"{written}/{len(payload)} bytes")
            if interval_ms and chunk_end < count:
                time.sleep(interval_ms / 1000.0)
            report_progress()

        while time.monotonic() < deadline:
            report_progress()
            now = time.monotonic()
            for i, data in enumerate(received):
                if direction_completed_at[i] is None and len(data) >= count:
                    direction_completed_at[i] = now
            if all(completed_at is not None for completed_at in direction_completed_at):
                break
            time.sleep(0.01)

        if all(completed_at is not None for completed_at in direction_completed_at) and settle_ms:
            stop.wait(min(settle_ms / 1000.0, max(deadline - time.monotonic(), 0.0)))
    except Exception as exc:
        errors.append(str(exc))
    finally:
        stop.set()
        for reader in readers:
            reader.join()

    report_progress(force=True)
    if all(completed_at is not None for completed_at in direction_completed_at):
        end = max(direction_completed_at)
    else:
        end = time.monotonic()
    duration_s = max(end - start, 1e-9)
    # Data received at TX came from RX, and vice versa.
    directions = {}
    for name, receive_index, send_index in (("tx_to_rx", 1, 0), ("rx_to_tx", 0, 1)):
        actual = bytes(received[receive_index])
        delivered = min(len(actual), count)
        expected_payload = expected[send_index]
        prefix_exact = actual[:count] == expected_payload[:len(actual[:count])]
        direction_end = direction_completed_at[receive_index] or end
        direction_duration_s = max(direction_end - start, 1e-9)
        directions[name] = {
            "sent_bytes": len(expected[send_index]),
            "received_bytes": len(actual),
            "delivered_bytes": delivered,
            "extra_bytes": max(len(actual) - count, 0),
            "short_bytes": max(count - len(actual), 0),
            "duration_s": round(direction_duration_s, 3),
            "goodput_Bps": round(delivered / direction_duration_s, 2),
            "prefix_exact": prefix_exact,
            "exact": actual == expected_payload,
        }

    return {
        "round": round_no + 1,
        "duration_s": round(duration_s, 3),
        "discarded_before_start": {"at_tx": drained[0], "at_rx": drained[1]},
        "input_quiet_before_start": input_quiet,
        "directions": directions,
        "timed_out": any(completed_at is None for completed_at in direction_completed_at),
        "errors": errors,
        "passed": all(direction["exact"] for direction in directions.values()) and not errors,
    }


def configure_parser(parser):
    parser.add_argument("--tx-port", required=True,
                        help="TX device data port, path or USB serial-number fragment")
    parser.add_argument("--rx-port", required=True,
                        help="RX device data port, path or USB serial-number fragment")
    parser.add_argument("--rounds", type=positive_int, default=3)
    parser.add_argument("--bytes", type=positive_int, default=4096, dest="byte_count",
                        help="bytes sent in each direction per round (default: 4096)")
    parser.add_argument("--chunk-size", type=positive_int, default=32)
    parser.add_argument("--interval-ms", type=nonnegative_int, default=50,
                        help="pause between chunks to pace USB input (default: 50; 0 disables)")
    parser.add_argument("--timeout", type=positive_int, default=60,
                        help="maximum seconds per round (default: 60)")
    parser.add_argument("--settle-ms", type=nonnegative_int, default=250,
                        help="extra receive time to detect duplicates (default: 250)")
    parser.add_argument("--report-interval", type=positive_float, default=1.0,
                        help="progress output interval in seconds (default: 1)")
    parser.add_argument("--format", choices=("human", "json"), default="human",
                        help="iperf-style output or one JSON record per round (default: human)")
    parser.add_argument("--baud", type=positive_int, default=DEFAULT_BAUD)
    return parser


def main(args=None, parser=None) -> int:
    if args is None:
        parser = argparse.ArgumentParser(
            description="measure verified bidirectional ZLRS data throughput")
        configure_parser(parser)
        args = parser.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial required: pip install pyserial", file=sys.stderr)
        return 2

    try:
        tx_path = resolve_port(args.tx_port)
        rx_path = resolve_port(args.rx_port)
        if tx_path == rx_path:
            raise ValueError("TX and RX must be different serial ports")
        ports = []
        try:
            for path in (tx_path, rx_path):
                ports.append(serial.Serial(path, args.baud, timeout=0.05, write_timeout=3))
        except Exception:
            for port in ports:
                port.close()
            raise
    except (OSError, ValueError, serial.SerialException) as exc:
        print(f"cannot open data ports: {exc}", file=sys.stderr)
        if (isinstance(exc, PermissionError) or getattr(exc, "errno", None) == errno.EACCES or
                "permission denied" in str(exc).lower()):
            print("On Linux/WSL, add your user to dialout with `sudo usermod -aG dialout $USER`, "
                  "then restart WSL and try again.", file=sys.stderr)
        return 2

    try:
        all_passed = True
        if args.format == "human":
            print(f"ZLRS bidirectional test: {args.byte_count} bytes per direction, "
                  f"{args.rounds} round(s), interval {args.report_interval:g} sec")
        for round_no in range(args.rounds):
            if args.format == "human":
                print(f"\nRound {round_no + 1}/{args.rounds}")
            result = run_round(ports, round_no, args.byte_count, args.chunk_size,
                               args.interval_ms, args.timeout, args.settle_ms,
                               args.report_interval, args.format == "human")
            if args.format == "json":
                print(json.dumps(result, sort_keys=True), flush=True)
            else:
                for name, direction in result["directions"].items():
                    status = "OK" if direction["exact"] else "MISMATCH"
                    print(f"  {name:7} {format_bytes(direction['received_bytes']):>12} / "
                          f"{format_bytes(direction['sent_bytes'])}  "
                          f"{format_rate(direction['delivered_bytes'], direction['duration_s'])}  "
                          f"{status}")
                    if not direction["exact"]:
                        detail = []
                        if direction["extra_bytes"]:
                            detail.append(f"{direction['extra_bytes']} extra bytes")
                        if direction["short_bytes"]:
                            detail.append(f"{direction['short_bytes']} bytes short")
                        detail.append("payload prefix matches" if direction["prefix_exact"]
                                      else "payload differs before the end")
                        print(f"           ({', '.join(detail)})")
                if result["errors"]:
                    print(f"  error: {'; '.join(result['errors'])}")
                if result["timed_out"]:
                    print("  timed out")
                print(f"  elapsed {result['duration_s']:.2f} sec — "
                      f"{'PASS' if result['passed'] else 'FAIL'}", flush=True)
            all_passed &= result["passed"]
            if not result["passed"]:
                break
        return 0 if all_passed else 1
    finally:
        for port in ports:
            port.close()


if __name__ == "__main__":
    raise SystemExit(main())
