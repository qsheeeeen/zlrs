#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the actual firmware with test data endpoints, then verify MCUboot rollback."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import serial

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tests/hardware"))
sys.path.insert(0, str(REPO / "tools"))
from unit.run import image_hash
from zlrs_cli import positive_seconds
from zlrs_update import image_hash_of
import zlrs_mgmt as mgmt

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
BENCH = re.compile(r"BENCH active=(\d+) target=(\d+) queued=(\d+) rx=(\d+) bad=(\d+) "
                   r"duration_us=(\d+) goodput_Bps=(\d+)")
RAW = re.compile(r"RAW ready=(\d+) tx=(\d+) rx=(\d+) bad=(\d+) duration_ms=(\d+)")


def wait_image(identity, expected, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if image_hash(identity) == expected:
                return
        except (OSError, RuntimeError, subprocess.SubprocessError):
            pass
        time.sleep(0.2)
    raise TimeoutError(f"Firmware {expected} did not return on {identity}")


def command(port, text):
    port.reset_input_buffer()
    port.write(b"\x03\r\n" + text.encode() + b"\r\n")
    output = bytearray()
    deadline = time.monotonic() + 0.5
    while time.monotonic() < deadline:
        try:
            output.extend(port.read(4096))
        except (OSError, serial.SerialException):
            if text == "test_radio restore":
                break
            raise
    return ANSI.sub("", output.decode(errors="replace"))


def debug_report(identity, name):
    path = f"/dev/serial/by-id/usb-zlrs_ZLRS_{identity}-if02"
    with serial.Serial(path, 115200, timeout=2) as port:
        def request(cmd):
            port.write(mgmt.encode(cmd))
            got, reply = mgmt.read_response(port)
            if got != mgmt.ACK or reply != bytes((cmd, 0)):
                raise RuntimeError(f"Management command failed on {identity}: {got}, {reply!r}")

        request(mgmt.CMD_ENTER)
        try:
            return mgmt.read_debug_report(port, mgmt.DEBUG_REPORTS[name]).decode()
        finally:
            request(mgmt.CMD_EXIT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="first board USB serial")
    parser.add_argument("--peer", required=True, help="second board USB serial")
    parser.add_argument("--image", type=Path,
                        default=REPO / "build/radio/radio/zephyr/zephyr.signed.bin")
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--bytes", type=int, default=262145)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=positive_seconds, default=60,
                        help="maximum seconds per DATA round (default: 60)")
    parser.add_argument("--raw", action="store_true", help="image must enable ZLRS_TEST_RAW_MS")
    args = parser.parse_args()
    if args.serial == args.peer or not 0 < args.bytes <= 16 * 1024 * 1024 or args.rounds < 1:
        parser.error("select two different boards, 1..16 MiB and at least one round")
    expected = image_hash_of(args.image)
    if expected is None:
        parser.error("invalid signed test image")
    identities = (args.serial, args.peer)
    previous = {identity: image_hash(identity) for identity in identities}
    record = {"previous": previous, "image": expected, "rounds": [], "restored": {}}
    installed = []
    consoles = {}
    try:
        for identity in identities:
            installed.append(identity)
            subprocess.run([sys.executable, str(REPO / "tools/zlrs.py"), "update",
                            "--serial", identity, "--image", str(args.image), "--no-wait",
                            "--timeout", "45"], check=True)
            # Raw mode waits inside radio init; management becomes ready after raw testing.
            if not args.raw:
                wait_image(identity, expected)
        with contextlib.ExitStack() as stack, ThreadPoolExecutor(2) as pool:
            for identity in identities:
                path = Path("/dev/serial/by-id") / f"usb-zlrs_ZLRS_{identity}-if04"
                deadline = time.monotonic() + 45
                while not path.exists():
                    if time.monotonic() > deadline:
                        raise TimeoutError(f"No console on {identity}")
                    time.sleep(0.1)
                consoles[identity] = stack.enter_context(serial.Serial(
                    str(path), 115200, timeout=0.02, write_timeout=2))

            def both(text):
                return dict(zip(identities, pool.map(
                    lambda identity: command(consoles[identity], text), identities)))

            def parse(text, pattern, names):
                values = {}
                for identity, output in both(text).items():
                    match = pattern.search(output)
                    if match is None:
                        raise RuntimeError(f"Incomplete {text} on {identity}: {output!r}")
                    values[identity] = dict(zip(names, map(int, match.groups())))
                return values

            if args.raw:
                names = ("ready", "tx", "rx", "bad", "duration_ms")
                ready = parse("test_radio status", RAW, names)
                if not all(value["ready"] for value in ready.values()):
                    raise RuntimeError("Both boards must wait for raw test start")
                both("test_radio start")
                time.sleep(max(value["duration_ms"] for value in ready.values()) / 1000 + 1)
                result = parse("test_radio status", RAW, names)
                record["raw"] = result
                if (not any(value["tx"] > 0 for value in result.values()) or
                        not any(value["rx"] > 0 for value in result.values()) or
                        any(value["bad"] for value in result.values())):
                    raise RuntimeError("Raw radio delivery failed")
                for identity in identities:
                    wait_image(identity, expected)
            else:
                def reports(name):
                    return dict(zip(identities, pool.map(
                        lambda identity: debug_report(identity, name), identities)))

                record["before"] = reports("radio")
                names = ("active", "target", "queued", "rx", "bad", "duration_us", "goodput_Bps")
                for number in range(args.rounds):
                    deadline = time.monotonic() + 20
                    while True:
                        arm = both(f"test_data arm {args.bytes}")
                        if all("BENCH arm: 0" in output for output in arm.values()):
                            break
                        if time.monotonic() > deadline:
                            raise TimeoutError("Data links did not become ready")
                    start = both("test_data start")
                    if not all("BENCH start: 0" in output for output in start.values()):
                        raise RuntimeError("Cannot start both data sources")
                    deadline = time.monotonic() + args.timeout
                    while True:
                        result = parse("test_data status", BENCH, names)
                        record["last_round"] = {"round": number + 1, "nodes": result}
                        if all(value["rx"] >= args.bytes for value in result.values()):
                            break
                        if time.monotonic() > deadline:
                            record["timeout_images"] = {}
                            for identity in identities:
                                try:
                                    record["timeout_images"][identity] = image_hash(identity)
                                except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                                    record["timeout_images"][identity] = str(error)
                            record["timeout_radio"] = reports("radio")
                            record["timeout_link"] = reports("link")
                            raise TimeoutError("RF data delivery timed out")
                    record["rounds"].append({"round": number + 1, "nodes": result})
                    print(json.dumps(record["rounds"][-1]), flush=True)
                    if not all(value["rx"] == args.bytes and value["queued"] == args.bytes and
                               value["bad"] == 0 for value in result.values()):
                        raise RuntimeError("RF data does not match the generated stream")
                    stopped = both("test_data stop")
                    if not all("BENCH stop: 0" in output for output in stopped.values()):
                        raise RuntimeError("Radio queue did not drain before stop")
                record["after"] = reports("radio")
            both("test_radio restore")
            consoles.clear()
    finally:
        # A failed test still requests rollback; the app confirmation timer is the fallback.
        for identity in installed:
            try:
                path = f"/dev/serial/by-id/usb-zlrs_ZLRS_{identity}-if04"
                with serial.Serial(path, 115200, timeout=0.02, write_timeout=2) as port:
                    command(port, "test_radio restore")
            except (OSError, serial.SerialException):
                pass
            try:
                wait_image(identity, previous[identity], timeout=125)
                record["restored"][identity] = True
            except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError):
                record["restored"][identity] = False
        args.log.write_text(json.dumps(record, indent=2) + "\n")
    if not all(record["restored"].get(identity) for identity in identities):
        raise RuntimeError("Previous firmware was not restored on both boards")
    print(f"Radio tests passed; previous firmware restored. Log: {args.log}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        print(f"Radio test error: {error}", file=sys.stderr)
        sys.exit(1)
