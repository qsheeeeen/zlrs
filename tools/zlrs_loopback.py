#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#
# One-sided DATA loopback capture. Send a deterministic byte stream through the
# DATA CDC and verify the stream returned by a UART loopback on the remote peer.
# If present, status is read through online queries on the management CDC.

import argparse
import csv
import datetime as dt
import glob
import json
import random
import sys
import time
from pathlib import Path
import zlrs_mgmt as mgmt

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


def matching_management(data_path: str) -> str | None:
    path = Path(data_path)
    suffix = "-if00"
    if not path.name.endswith(suffix):
        return None
    candidate = path.with_name(path.name[:-len(suffix)] + "-if02")
    return str(candidate) if candidate.exists() else None


def sample_status(client) -> dict:
    response = client.request(mgmt.CMD_STATUS)
    status = mgmt.decode_status(response["status"])
    metrics = {
        "role": "Leader" if status.role == 0 else "Follower",
        "band": "sub-GHz" if status.flags & 8 else "2.4 GHz",
        "modulation": mgmt.MODULATION_NAMES.get(status.data_phy, f"unknown profile ({status.data_phy})"),
        "state": status.state, "paired": bool(status.flags & 2),
        "tx_frames": status.tx_frames, "rx_frames": status.rx_frames,
        "retries": status.tx_retries, "crc_errors": status.rx_crc_errors,
        "tag_errors": status.rx_bad_tag, "radio_failures": status.radio_fail,
        "missed_slots": status.missed_slots, "rf_drop_bytes": status.data_drop_bytes,
    }
    for prefix in ("", "peer_"):
        rssi, lq = getattr(status, prefix + "rssi_dbm"), getattr(status, prefix + "lq_percent")
        if rssi:
            metrics[prefix + "rssi_dbm"] = rssi
        if lq <= 100 and (not prefix or rssi):
            metrics[prefix + "lq_percent"] = lq
    return {
        "captured_at": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "status": mgmt.status_text(status, system=response),
        "metrics": metrics,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True,
                        help="TX USB serial fragment or explicit DATA CDC path")
    parser.add_argument("--management-port", help="management CDC path or serial fragment (auto-detected)")
    parser.add_argument("--no-status", action="store_true",
                        help="skip online management status sampling")
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
    management_path = None
    if not args.no_status:
        if args.management_port:
            management_path = resolve_port(args.management_port, "if02")
        else:
            management_path = matching_management(data_path)
    stamp = dt.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    out_dir = args.out_dir or Path("artifacts") / f"loopback-{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    data = None
    management = None
    client = None
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
        if management_path is not None:
            management = mgmt.ManagementClient(management_path, timeout=1)
            client = management.__enter__()
        time.sleep(0.2)
        # Clear only stale host-side input before a fresh deterministic run.
        data.reset_input_buffer()
        print(f"DATA port={data_path} management={management_path or 'disabled'}", flush=True)
        if client is not None:
            pre = sample_status(client)
            status_samples.append(pre)
            print(pre["metrics"], flush=True)
        else:
            print("No management port detected; collecting DATA loopback only.", flush=True)
        print(f"Capturing {args.duration:g}s; move RX now. Output: {out_dir}", flush=True)

        rng = random.Random(SEED)
        start = time.monotonic()
        end_at = start + args.duration
        next_send = start
        next_status = start + args.status_interval if client is not None else float("inf")
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
            if client is not None and now >= next_status:
                sample = sample_status(client)
                status_samples.append(sample)
                parsed = sample["metrics"]
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

        if client is not None:
            post = sample_status(client)
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
        if management is not None:
            management.__exit__(None, None, None)

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
            "management_port": management_path,
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
