#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
# One-sided DATA loopback capture. Send a deterministic byte stream through the
# DATA CDC and verify the stream returned by a UART loopback on the remote peer.
# If present, debug status is read from the non-pausing console CDC.

import argparse
import csv
import datetime as dt
import glob
import json
import random
import re
import sys
import time
from pathlib import Path

BY_ID_DIR = "/dev/serial/by-id"
SEED = 0x5A4C5253


def resolve_port(value: str, interface: str) -> str:
    if "/" in value or "\\" in value:
        return value
    matches = [p for p in glob.glob(f"{BY_ID_DIR}/*-{interface}")
               if value.lower() in Path(p).name.lower()]
    if len(matches) > 1:
        raise ValueError(f"{value!r} matches several {interface} ports: {matches}")
    return matches[0] if matches else value


def matching_console(data_path: str) -> str | None:
    path = Path(data_path)
    suffix = "-if00"
    if not path.name.endswith(suffix):
        return None
    candidate = path.with_name(path.name[:-len(suffix)] + "-if04")
    return str(candidate) if candidate.exists() else None


def shell_command(port, command: str, timeout_s: float = 1.5) -> str:
    port.reset_input_buffer()
    port.write(command.encode("ascii") + b"\r\n")
    deadline = time.monotonic() + timeout_s
    chunks = bytearray()
    last_byte = time.monotonic()
    while time.monotonic() < deadline:
        data = port.read(4096)
        if data:
            chunks.extend(data)
            last_byte = time.monotonic()
            if b"uart:~$" in chunks:
                break
        elif chunks and time.monotonic() - last_byte >= 0.15:
            break
    return chunks.decode("utf-8", errors="replace")


def sample_status(console) -> dict:
    return {
        "captured_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "status": shell_command(console, "zlrs status"),
        "ports": shell_command(console, "zlrs ports"),
    }


def parse_status(sample: dict) -> dict:
    text = sample["status"] + "\n" + sample["ports"]
    patterns = {
        "role": r"role\s+:\s+(.+)",
        "band": r"band\s+:\s+(.+)",
        "link": r"link\s+:\s+state (\d+), lq (\d+)%, paired (\d+)",
        "rssi_dbm": r"signal\s+:\s+RSSI (-?\d+) dBm",
        "peer_rssi_dbm": r"peer\s+:\s+RSSI (-?\d+) dBm, lq (\d+)%",
        "frames": r"frames\s+:\s+TX (\d+), RX (\d+), retry (\d+)",
        "errors": r"errors\s+:\s+CRC (\d+), tag (\d+), radio (\d+), missed slots (\d+)",
        "rf_drop_bytes": r"data\s+:\s+RF drop (\d+) B",
    }
    result = {"captured_at": sample["captured_at"]}
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match:
            continue
        values = match.groups()
        if key in ("role", "band"):
            result[key] = values[0].strip()
        elif key == "link":
            result.update(state=int(values[0]), lq_percent=int(values[1]),
                          paired=bool(int(values[2])))
        elif key == "peer_rssi_dbm":
            result[key] = int(values[0])
            result["peer_lq_percent"] = int(values[1])
        elif key == "frames":
            result.update(tx_frames=int(values[0]), rx_frames=int(values[1]),
                          retries=int(values[2]))
        elif key == "errors":
            result.update(crc_errors=int(values[0]), tag_errors=int(values[1]),
                          radio_failures=int(values[2]), missed_slots=int(values[3]))
        else:
            result[key] = int(values[0])
    # Current grouped output; retain the legacy parser for older device firmware.
    grouped = dict(re.findall(r"^\s*([A-Za-z][A-Za-z /]*?)\s*:\s*(.*?)\s*$",
                              text, re.MULTILINE))
    for label, key in (("Role", "role"), ("Active band", "band"),
                       ("Modulation", "modulation")):
        if label in grouped:
            result[key] = grouped[label]
    state_names = ("unconfigured", "unpaired", "pairing", "searching", "connected")
    if grouped.get("State") in state_names:
        result["state"] = state_names.index(grouped["State"])
    if grouped.get("Paired") in ("yes", "no"):
        result["paired"] = grouped["Paired"] == "yes"
    for label, key, unit in (("Local RSSI", "rssi_dbm", "dBm"),
                             ("Peer RSSI", "peer_rssi_dbm", "dBm"),
                             ("Local LQ", "lq_percent", "%"),
                             ("Peer LQ", "peer_lq_percent", "%"),
                             ("ARQ retries", "retries", ""),
                             ("CRC errors", "crc_errors", ""),
                             ("Auth errors", "tag_errors", ""),
                             ("Radio failures", "radio_failures", ""),
                             ("Missed slots", "missed_slots", ""),
                             ("Dropped data", "rf_drop_bytes", "bytes")):
        match = re.fullmatch(r"(-?\d+)\s*" + re.escape(unit), grouped.get(label, ""))
        if match:
            result[key] = int(match[1])
    match = re.fullmatch(r"TX (\d+), RX (\d+)", grouped.get("RF frames", ""))
    if match:
        result.update(tx_frames=int(match[1]), rx_frames=int(match[2]))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True,
                        help="TX USB serial fragment or explicit DATA CDC path")
    parser.add_argument("--console", help="debug console path or serial fragment (auto-detected if available)")
    parser.add_argument("--no-status", action="store_true",
                        help="skip debug-console status sampling")
    parser.add_argument("--duration", type=float, default=120.0)
    parser.add_argument("--chunk-size", type=int, default=32)
    parser.add_argument("--interval-ms", type=int, default=50)
    parser.add_argument("--status-interval", type=float, default=5.0)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    if (args.duration <= 0 or args.chunk_size <= 0 or args.interval_ms < 0 or
            args.status_interval <= 0):
        parser.error("duration, chunk-size and status-interval must be positive; "
                     "interval-ms must be nonnegative")

    try:
        import serial
    except ImportError:
        print("pyserial is required", file=sys.stderr)
        return 2

    data_path = resolve_port(args.device, "if00")
    console_path = None
    if not args.no_status:
        if args.console:
            console_path = resolve_port(args.console, "if04")
        else:
            console_path = matching_console(data_path)
    stamp = dt.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    out_dir = args.out_dir or Path("artifacts") / f"loopback-{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    data = None
    console = None
    transmitted = bytearray()
    received = bytearray()
    status_samples = []
    timeline = []
    mismatch_bytes = 0
    first_mismatch = None
    write_errors = []
    interrupted = False

    try:
        data = serial.Serial(data_path, 115200, timeout=0.01, write_timeout=2)
        if console_path is not None:
            console = serial.Serial(console_path, 115200, timeout=0.05, write_timeout=2)
        time.sleep(0.2)
        # Clear only stale host-side input before a fresh deterministic run.
        data.reset_input_buffer()
        print(f"DATA port={data_path} console={console_path or 'disabled'}", flush=True)
        if console is not None:
            pre = sample_status(console)
            status_samples.append(pre)
            print(parse_status(pre), flush=True)
        else:
            print("No debug console detected; collecting DATA loopback only.", flush=True)
        print(f"Capturing {args.duration:g}s; move RX now. Output: {out_dir}", flush=True)

        rng = random.Random(SEED)
        start = time.monotonic()
        end_at = start + args.duration
        next_send = start
        next_status = start + args.status_interval if console is not None else float("inf")
        next_timeline = start + 1.0
        last_progress = start
        rx_offset = 0
        while time.monotonic() < end_at:
            now = time.monotonic()
            if now >= next_send:
                chunk = rng.randbytes(args.chunk_size)
                try:
                    written = data.write(chunk)
                    transmitted.extend(chunk[:written])
                    if written != len(chunk):
                        write_errors.append(f"short write {written}/{len(chunk)} at {len(transmitted)}")
                except Exception as exc:
                    write_errors.append(f"write at {len(transmitted)}: {exc}")
                    break
                next_send = max(next_send + args.interval_ms / 1000.0,
                                time.monotonic() + args.interval_ms / 1000.0)
                if args.interval_ms == 0:
                    next_send = time.monotonic()

            chunk = data.read(4096)
            if chunk:
                expected = transmitted[rx_offset:rx_offset + len(chunk)]
                compared = min(len(expected), len(chunk))
                for index in range(compared):
                    if expected[index] != chunk[index]:
                        mismatch_bytes += 1
                        if first_mismatch is None:
                            first_mismatch = rx_offset + index
                if compared < len(chunk):
                    mismatch_bytes += len(chunk) - compared
                    if first_mismatch is None:
                        first_mismatch = rx_offset + compared
                received.extend(chunk)
                rx_offset += len(chunk)

            now = time.monotonic()
            if console is not None and now >= next_status:
                sample = sample_status(console)
                status_samples.append(sample)
                parsed = parse_status(sample)
                print(f"status {sample['captured_at']}: {parsed}", flush=True)
                next_status = now + args.status_interval
            if now >= next_timeline:
                timeline.append({
                    "elapsed_s": round(now - start, 3),
                    "tx_bytes": len(transmitted),
                    "rx_bytes": len(received),
                    "mismatch_bytes": mismatch_bytes,
                })
                next_timeline = now + 1.0
            if now - last_progress >= 10.0:
                print(f"progress {now-start:.0f}s TX={len(transmitted)} B "
                      f"RX={len(received)} B mismatches={mismatch_bytes}", flush=True)
                last_progress = now

        # Drain the return path after the last transmit for up to three seconds.
        drain_until = time.monotonic() + 3.0
        while time.monotonic() < drain_until:
            chunk = data.read(4096)
            if not chunk:
                if len(received) >= len(transmitted):
                    break
                continue
            expected = transmitted[rx_offset:rx_offset + len(chunk)]
            compared = min(len(expected), len(chunk))
            for index in range(compared):
                if expected[index] != chunk[index]:
                    mismatch_bytes += 1
                    if first_mismatch is None:
                        first_mismatch = rx_offset + index
            if compared < len(chunk):
                mismatch_bytes += len(chunk) - compared
                if first_mismatch is None:
                    first_mismatch = rx_offset + compared
            received.extend(chunk)
            rx_offset += len(chunk)

        if console is not None:
            post = sample_status(console)
            status_samples.append(post)
    except KeyboardInterrupt:
        interrupted = True
        print("Interrupted; preserving captured data", flush=True)
    except Exception as exc:
        write_errors.append(str(exc))
        print(f"capture error: {exc}", file=sys.stderr, flush=True)
    finally:
        if data is not None:
            data.close()
        if console is not None:
            console.close()

        (out_dir / "transmitted.bin").write_bytes(transmitted)
        (out_dir / "received.bin").write_bytes(received)
        (out_dir / "status.json").write_text(json.dumps(status_samples, indent=2), encoding="utf-8")
        with (out_dir / "timeline.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=["elapsed_s", "tx_bytes", "rx_bytes",
                                                        "mismatch_bytes"])
            writer.writeheader()
            writer.writerows(timeline)

        compared = min(len(transmitted), len(received))
        # Full byte-for-byte comparison also catches a shifted stream after any insertion/loss.
        common_exact = transmitted[:compared] == received[:compared]
        summary = {
            "device": args.device,
            "data_port": data_path,
            "console_port": console_path,
            "duration_s_requested": args.duration,
            "chunk_size": args.chunk_size,
            "interval_ms": args.interval_ms,
            "seed": SEED,
            "tx_bytes": len(transmitted),
            "rx_bytes": len(received),
            "short_bytes": max(len(transmitted) - len(received), 0),
            "extra_bytes": max(len(received) - len(transmitted), 0),
            "mismatch_bytes_at_arrival": mismatch_bytes,
            "first_mismatch_offset": first_mismatch,
            "common_prefix_exact": common_exact,
            "full_echo_exact": transmitted == received,
            "interrupted": interrupted,
            "write_errors": write_errors,
            "status_samples": len(status_samples),
            "output_dir": str(out_dir),
        }
        (out_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(json.dumps(summary, indent=2), flush=True)
        print(f"Artifacts: {out_dir}", flush=True)
    return 0 if not write_errors and transmitted == received and not interrupted else 1


if __name__ == "__main__":
    raise SystemExit(main())
