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
RAW = re.compile(r"RAW ready=(\d+) tx=(\d+) tx_failed=(\d+) rx=(\d+) expected=(\d+) "
                 r"bad=(\d+) frequency_mhz=(\d+) payload_bytes=(\d+) duration_ms=(\d+)")
CHANNEL = re.compile(r"CHANNEL frequency_mhz=(\d+) tx=(\d+) tx_failed=(\d+) "
                     r"rx=(\d+) empty=(\d+) crc=(\d+)")
ENTROPY = re.compile(r"ENTROPY active=(\d+) pending=(\d+) calls=(\d+) empty=(\d+) "
                     r"max_us=(\d+) restores=(\d+)")


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
    with mgmt.management_session(path, 115200, 2) as client:
        return mgmt.read_debug_report(client, mgmt.DEBUG_REPORTS[name]).decode()


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
    parser.add_argument("--profiles", action="store_true",
                        help="sweep 2.4 GHz FLRC/LoRa profiles down and up (auto-mode peers)")
    parser.add_argument("--entropy-stall-ms", type=int, default=0,
                        help="stall both TRNGs during one sustained DATA round (3500..10000 ms)")
    args = parser.parse_args()
    if args.serial == args.peer or not 0 < args.bytes <= 16 * 1024 * 1024 or args.rounds < 1:
        parser.error("select two different boards, 1..16 MiB and at least one round")
    if args.profiles and args.raw:
        parser.error("--profiles cannot be combined with --raw")
    if args.entropy_stall_ms and (not 3500 <= args.entropy_stall_ms <= 10000 or
                                  args.profiles or args.raw or args.rounds != 1):
        parser.error("--entropy-stall-ms needs 3500..10000 ms, one round, no raw/profile sweep")
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

            def reports(name):
                return dict(zip(identities, pool.map(
                    lambda identity: debug_report(identity, name), identities)))

            def channels():
                names = ("frequency_mhz", "tx", "tx_failed", "rx", "empty", "crc")
                result = {}
                for identity, output in both("test_radio channels").items():
                    rows = [dict(zip(names, map(int, match.groups())))
                            for match in CHANNEL.finditer(output)]
                    if [row["frequency_mhz"] for row in rows] != list(range(2402, 2481)):
                        raise RuntimeError(f"Incomplete channel report on {identity}: {output!r}")
                    result[identity] = rows
                return result

            if args.raw:
                names = ("ready", "tx", "tx_failed", "rx", "expected", "bad",
                         "frequency_mhz", "payload_bytes", "duration_ms")
                ready = parse("test_radio status", RAW, names)
                if not all(value["ready"] for value in ready.values()):
                    raise RuntimeError("Both boards must wait for raw test start")
                both("test_radio start")
                time.sleep(max(value["duration_ms"] for value in ready.values()) / 1000 + 1)
                result = parse("test_radio status", RAW, names)
                for value in result.values():
                    if value["expected"] > 0:
                        value["quality_percent"] = value["rx"] * 100 / value["expected"]
                record["raw"] = result
                record["after"] = reports("radio")
                if (not any(value["tx"] > 0 for value in result.values()) or
                        not any(value["rx"] > 0 for value in result.values()) or
                        any(value["bad"] for value in result.values())):
                    raise RuntimeError("Raw radio delivery failed")
                for identity in identities:
                    wait_image(identity, expected)
            else:
                record["before"] = reports("radio")
                record["before_link"] = reports("link")
                record["before_channels"] = channels()
                record["observations"] = []
                names = ("active", "target", "queued", "rx", "bad", "duration_us", "goodput_Bps")
                profile_names = ("FLRC 2.6 Mbps", "FLRC 650 kbps", "FLRC 325 kbps",
                                 "LoRa SF6/BW1MHz", "LoRa SF7/BW1MHz")
                steps = [(None, number) for number in range(args.rounds)]
                if args.profiles:
                    roles = both("zlrs status")
                    leaders = [identity for identity, output in roles.items()
                               if re.search(r"Role\s*:\s*Leader", output)]
                    if len(leaders) != 1:
                        raise RuntimeError("Profile sweep needs exactly one Leader")
                    leader = leaders[0]
                    steps = [(phy, number) for phy in (0, 1, 2, 3, 4, 3, 2, 1, 0)
                             for number in range(args.rounds)]
                for phy, number in steps:
                    if phy is not None:
                        output = command(consoles[leader], f"zlrs phy {phy}")
                        if f"queued profile {phy}:" not in output:
                            raise RuntimeError(f"Profile request failed: {output!r}")
                        deadline = time.monotonic() + 45
                        while True:
                            observed = both("zlrs status")
                            ready = all(re.search(r"Modulation\s*:\s*" + re.escape(profile_names[phy]), text)
                                        and re.search(r"State\s*:\s*connected\b", text)
                                        and re.search(r"Band mode\s*:\s*auto\b", text)
                                        and re.search(r"Capture\s*:\s*ready\b", text)
                                        and re.search(r"Transition\s*:\s*none\b", text)
                                        for text in observed.values())
                            if ready:
                                break
                            if time.monotonic() >= deadline:
                                record["profile_timeout"] = {"phy": phy, "status": observed}
                                record["profile_radio"] = reports("radio")
                                record["profile_link"] = reports("link")
                                raise TimeoutError(f"Both peers did not capture profile {phy}")
                            time.sleep(0.2)
                    deadline = time.monotonic() + 20
                    while True:
                        arm = both(f"test_data arm {args.bytes}")
                        if all("BENCH arm: 0" in output for output in arm.values()):
                            break
                        if time.monotonic() > deadline:
                            record["arm"] = arm
                            record["timeout_radio"] = reports("radio")
                            record["timeout_link"] = reports("link")
                            raise TimeoutError("Data links did not become ready")
                    start = both("test_data start")
                    if not all("BENCH start: 0" in output for output in start.values()):
                        raise RuntimeError("Cannot start both data sources")
                    fault = None
                    entropy_names = ("active", "pending", "calls", "empty", "max_us", "restores")
                    if args.entropy_stall_ms:
                        queued = both(f"test_radio entropy {args.entropy_stall_ms}")
                        if not all(f"ENTROPY queued_ms={args.entropy_stall_ms}" in output
                                   for output in queued.values()):
                            raise RuntimeError(f"Could not queue TRNG stalls: {queued!r}")
                        fault = {"duration_ms": args.entropy_stall_ms, "active_samples": []}
                        record["entropy_stall"] = fault
                    deadline = time.monotonic() + args.timeout
                    started = time.monotonic()
                    observe_at = started
                    while True:
                        result = parse("test_data status", BENCH, names)
                        record["last_round"] = {"round": number + 1, "nodes": result}
                        if fault is not None:
                            entropy = parse("test_radio entropy", ENTROPY, entropy_names)
                            fault["last"] = entropy
                            if all(value["active"] for value in entropy.values()):
                                fault["active_samples"].append(result)
                        if time.monotonic() >= observe_at:
                            record["observations"].append({
                                "round": number + 1,
                                "seconds": round(time.monotonic() - started, 3),
                                "status": both("zlrs status"),
                            })
                            observe_at = time.monotonic() + 2
                        fault_done = fault is None or all(value["restores"] == 1 and
                                                         not value["active"] and not value["pending"]
                                                         for value in fault["last"].values())
                        if all(value["rx"] >= args.bytes for value in result.values()) and fault_done:
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
                    record["rounds"].append({"round": number + 1, "phy": phy, "nodes": result})
                    record["rounds"][-1]["status"] = both("zlrs status")
                    print(json.dumps(record["rounds"][-1]), flush=True)
                    if not all(value["rx"] == args.bytes and value["queued"] == args.bytes and
                               value["bad"] == 0 for value in result.values()):
                        raise RuntimeError("RF data does not match the generated stream")
                    if fault is not None:
                        samples = fault["active_samples"]
                        if len(samples) < 2 or not all(samples[-1][identity]["rx"] >
                                                       samples[0][identity]["rx"]
                                                       for identity in identities):
                            raise RuntimeError("No bidirectional DATA progress observed during TRNG stalls")
                        if not all(value["calls"] > 0 and value["empty"] == value["calls"] and
                                   value["max_us"] < 1000 for value in fault["last"].values()):
                            raise RuntimeError("RF nonce probes blocked or accepted bytes from stalled TRNGs")
                    stopped = both("test_data stop")
                    if not all("BENCH stop: 0" in output for output in stopped.values()):
                        raise RuntimeError("Radio queue did not drain before stop")
                record["after"] = reports("radio")
                record["after_link"] = reports("link")
                record["after_channels"] = channels()
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
