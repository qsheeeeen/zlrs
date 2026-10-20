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
import threading
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
SAVE_FAULT = re.compile(r"SAVE_FAULT active=(\d+) pending=(\d+) calls=(\d+) first_ms=(\d+) "
                        r"last_ms=(\d+) min_gap_ms=(\d+) restores=(\d+)")


def timing_snapshot(report):
    names = ("prepare IRQ", "owner wake", "serial pump", "protocol build", "packet total",
             "radio prepare", "RX protocol", "RX completion")
    phases = {}
    for name in names:
        row = re.search(r"^\s+" + re.escape(name) +
                        r"\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$",
                        report, re.MULTILINE)
        if row is None:
            if name in names[:2] and re.search(re.escape(name) + r"\s+no samples", report):
                phases[name] = {"samples": 0}
                continue
            raise RuntimeError(f"Missing active timing samples for {name}")
        values = dict(zip(("samples", "mean_us", "p50_bound_us", "p95_bound_us",
                           "p99_bound_us", "max_us"), map(int, row.groups())))
        if not (values["samples"] > 0 and values["mean_us"] <= values["max_us"] and
                values["p50_bound_us"] <= values["p95_bound_us"] <=
                values["p99_bound_us"] <= values["max_us"]):
            raise RuntimeError(f"Inconsistent {name} timing statistics: {values}")
        phases[name] = values
    epoch = re.search(r"Epoch (\d+)\s+Since (\d+) ms", report)
    pauses = re.search(r"Pauses\s*:\s*(\d+)\s+Total (\d+) us\s+Max (\d+) us", report)
    catchup = re.search(r"Skipped after pause:\s*(\d+) frames\s+Without pause:\s*(\d+) frames", report)
    totals = re.search(r"Missed / skipped\s*:\s*\d+ boundaries\s+(\d+) frames", report)
    if not all((epoch, pauses, catchup, totals)):
        raise RuntimeError("Incomplete timing epoch or maintenance statistics")
    after, other = map(int, catchup.groups())
    if after + other != int(totals[1]):
        raise RuntimeError("Maintenance/runtime frame catch-up does not match its total")
    return {"epoch": int(epoch[1]), "started_ms": int(epoch[2]), "phases": phases,
            "maintenance": dict(zip(("count", "total_us", "max_us"), map(int, pauses.groups()))),
            "skipped_after_maintenance": after, "skipped_without_maintenance": other}


def wait_image(identity, expected, timeout=45):
    deadline = time.monotonic() + timeout
    observed = "no response"
    while time.monotonic() < deadline:
        try:
            observed = image_hash(identity)
            if observed == expected:
                return
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            observed = str(error)
        time.sleep(0.2)
    raise TimeoutError(f"Firmware {expected} did not return on {identity}; last observed: {observed}")


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


class OnlineObservers:
    """Poll each management CDC independently while the physical DATA links run."""

    def __init__(self, identities, hz):
        self.hz = hz
        self.done = threading.Event()
        self.records = {identity: {"latency_ms": [], "states": set(), "phys": set(),
                                   "hardware_samples": set(), "errors": []}
                        for identity in identities}
        self.threads = [threading.Thread(target=self.observe, args=(identity,))
                        for identity in identities] if hz else []
        self.stopped = False
        for thread in self.threads:
            thread.start()

    def observe(self, identity):
        record = self.records[identity]
        try:
            path = f"/dev/serial/by-id/usb-zlrs_ZLRS_{identity}-if02"
            with serial.Serial(path, 115200, timeout=2, write_timeout=2) as port:
                client = mgmt.ManagementClient(port)
                while not self.done.is_set():
                    before = time.monotonic()
                    payload = client.request(mgmt.CMD_OBSERVE, response=mgmt.OBSERVE_RESP)
                    if len(payload) != mgmt.OBSERVE_HEADER_LEN + mgmt.STATUS_LEN:
                        raise RuntimeError("Invalid online observation length")
                    status = mgmt.decode_status(payload[mgmt.OBSERVE_HEADER_LEN:])
                    sample = {"uptime_ms": int.from_bytes(payload[:4], "little"),
                              "tx_frames": status.tx_frames, "rx_frames": status.rx_frames,
                              "missed_slots": status.missed_slots, "radio_fail": status.radio_fail}
                    record.setdefault("first", sample)
                    record["last"] = sample
                    record["latency_ms"].append((time.monotonic() - before) * 1000)
                    record["states"].add(status.state)
                    record["phys"].add(status.data_phy)
                    record["hardware_samples"].add(int.from_bytes(payload[4:8], "little"))
                    if not status.flags & 4:
                        raise RuntimeError("Online query observed a retired DATA session")
                    self.done.wait(max(0, 1 / self.hz - (time.monotonic() - before)))
        except (OSError, ValueError, RuntimeError, TimeoutError) as error:
            record["errors"].append(str(error))

    def stop(self):
        if not self.stopped:
            self.done.set()
            for thread in self.threads:
                thread.join(timeout=5)
                if thread.is_alive():
                    raise RuntimeError("Online status observer did not stop")
            for record in self.records.values():
                latencies = sorted(record.pop("latency_ms"))
                record["queries"] = len(latencies)
                if latencies:
                    record["latency_ms"] = {"p50": latencies[len(latencies) // 2],
                                            "p99": latencies[min(len(latencies) - 1,
                                                                len(latencies) * 99 // 100)],
                                            "max": latencies[-1]}
                for name in ("states", "phys", "hardware_samples"):
                    record[name] = sorted(record[name])
            self.stopped = True
        return self.records

    def verify(self, phy):
        if self.hz and not all(record["queries"] >= 2 and not record["errors"] and
                               record["states"] == [4] and
                               len(record["hardware_samples"]) == 1 and
                               (phy is None or record["phys"] == [phy])
                               for record in self.records.values()):
            raise RuntimeError(f"Online status interrupted DATA or refreshed hardware: {self.records}")


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
    parser.add_argument("--phy", type=int, choices=range(5),
                        help="hold one automatic-mode PHY for a repeatable DATA measurement")
    parser.add_argument("--status-poll-hz", type=int, nargs="+", default=[0],
                        help="0..100 queries/s; one value or one per round, e.g. 0 50 0")
    parser.add_argument("--timing", action="store_true",
                        help="validate per-phase timing histograms and maintenance totals after each round")
    parser.add_argument("--entropy-stall-ms", type=int, default=0,
                        help="stall both TRNGs during one sustained DATA round (3500..10000 ms)")
    parser.add_argument("--pairing-save-fault-ms", type=int, default=0,
                        help="inject pairing-store EIOs during DATA (4000..30000 ms, one round)")
    args = parser.parse_args()
    if args.serial == args.peer or not 0 < args.bytes <= 16 * 1024 * 1024 or args.rounds < 1:
        parser.error("select two different boards, 1..16 MiB and at least one round")
    if args.profiles and args.raw:
        parser.error("--profiles cannot be combined with --raw")
    if (any(not 0 <= rate <= 100 for rate in args.status_poll_hz) or
            len(args.status_poll_hz) not in (1, args.rounds) or
            (args.phy is not None and (args.profiles or args.raw))):
        parser.error("select 0..100 status queries/s and only one of --phy/--profiles/--raw")
    if args.raw and (any(args.status_poll_hz) or args.timing):
        parser.error("online status polling and timing histograms require DATA mode")
    if args.entropy_stall_ms and (not 3500 <= args.entropy_stall_ms <= 10000 or
                                  args.profiles or args.raw or args.rounds != 1 or args.pairing_save_fault_ms):
        parser.error("--entropy-stall-ms needs 3500..10000 ms, one round, no raw/profile sweep")
    if args.pairing_save_fault_ms and (not 4000 <= args.pairing_save_fault_ms <= 30000 or
                                       args.profiles or args.raw or args.rounds != 1):
        parser.error("--pairing-save-fault-ms needs 4000..30000 ms, one round, no raw/profile sweep")
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
                steps = [(args.phy, number) for number in range(args.rounds)]
                if args.profiles or args.phy is not None:
                    roles = both("zlrs status")
                    leaders = [identity for identity, output in roles.items()
                               if re.search(r"Role\s*:\s*Leader", output)]
                    if len(leaders) != 1:
                        raise RuntimeError("Profile sweep needs exactly one Leader")
                    leader = leaders[0]
                    if args.profiles:
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
                    poll_hz = args.status_poll_hz[number % len(args.status_poll_hz)]
                    observers = OnlineObservers(identities, poll_hz)
                    stack.callback(observers.stop)
                    fault = None
                    fault_command = "entropy" if args.entropy_stall_ms else "save-fault"
                    fault_pattern = ENTROPY if args.entropy_stall_ms else SAVE_FAULT
                    fault_names = (("active", "pending", "calls", "empty", "max_us", "restores")
                                   if args.entropy_stall_ms else
                                   ("active", "pending", "calls", "first_ms", "last_ms", "min_gap_ms", "restores"))
                    fault_ms = args.entropy_stall_ms or args.pairing_save_fault_ms
                    if fault_ms:
                        queued = both(f"test_radio {fault_command} {fault_ms}")
                        marker = "ENTROPY" if args.entropy_stall_ms else "SAVE_FAULT"
                        if not all(f"{marker} queued_ms={fault_ms}" in output
                                   for output in queued.values()):
                            raise RuntimeError(f"Could not queue {fault_command}: {queued!r}")
                        fault = {"duration_ms": fault_ms, "active_samples": []}
                        record["entropy_stall" if args.entropy_stall_ms else "pairing_save_fault"] = fault
                    deadline = time.monotonic() + args.timeout
                    started = time.monotonic()
                    observe_at = started
                    while True:
                        result = parse("test_data status", BENCH, names)
                        record["last_round"] = {"round": number + 1, "nodes": result}
                        if fault is not None:
                            observed_fault = parse(f"test_radio {fault_command}", fault_pattern, fault_names)
                            fault["last"] = observed_fault
                            if all(value["active"] for value in observed_fault.values()):
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
                    record["rounds"][-1]["status_poll_hz"] = poll_hz
                    record["rounds"][-1]["online_status"] = observers.stop()
                    observers.verify(phy)
                    record["rounds"][-1]["status"] = both("zlrs status")
                    if args.timing:
                        record["rounds"][-1]["timing"] = {
                            identity: timing_snapshot(report)
                            for identity, report in reports("radio").items()}
                    print(json.dumps(record["rounds"][-1]), flush=True)
                    if not all(value["rx"] == args.bytes and value["queued"] == args.bytes and
                               value["bad"] == 0 for value in result.values()):
                        raise RuntimeError("RF data does not match the generated stream")
                    if fault is not None:
                        samples = fault["active_samples"]
                        if len(samples) < 2 or not all(samples[-1][identity]["rx"] >
                                                       samples[0][identity]["rx"]
                                                       for identity in identities):
                            raise RuntimeError(f"No bidirectional DATA progress during {fault_command}")
                        if args.entropy_stall_ms and not all(
                                value["calls"] > 0 and value["empty"] == value["calls"] and
                                value["max_us"] < 1000 for value in fault["last"].values()):
                            raise RuntimeError("RF nonce probes blocked or accepted bytes from stalled TRNGs")
                        if args.pairing_save_fault_ms and not all(
                                2 <= value["calls"] <= 5 and value["min_gap_ms"] >= 1000
                                for value in fault["last"].values()):
                            raise RuntimeError(f"Pairing save did not back off: {fault['last']}")
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
