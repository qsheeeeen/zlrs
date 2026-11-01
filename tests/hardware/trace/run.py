#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise bounded captures, download with smpmgr, and decode with Zephyr's viewer."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import serial

REPO = Path(__file__).resolve().parents[3]
CLI = REPO / "tools/zlrs.py"
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
sys.path.insert(0, str(REPO / "tests/hardware"))
from unit.run import image_hash
sys.path.insert(0, str(REPO / "tools"))
from zlrs_update import image_hash_of


def wait_hash(identity, expected, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if image_hash(identity) == expected:
                return
        except (OSError, RuntimeError, subprocess.SubprocessError):
            pass
        time.sleep(0.5)
    raise TimeoutError(f"Device {identity} did not return to {expected}")


class Acceptance:
    def __init__(self, args):
        self.args = args
        self.console = f"/dev/serial/by-id/usb-zlrs_ZLRS_{args.serial}-if04"
        self.output = args.out
        self.output.mkdir(parents=True, exist_ok=True)
        self.log = self.output / "console.log"
        self.result = {"serial": args.serial, "captures": []}
        self.smpmgr = str(Path(sys.executable).with_name("smpmgr"))
        self.metadata = args.zephyr / "subsys/tracing/ctf/tsdl/metadata"
        self.viewer_path = args.zephyr / "scripts/tracing/trace_viewer.py"
        spec = importlib.util.spec_from_file_location("zephyr_trace_viewer", self.viewer_path)
        self.viewer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.viewer)

    def command(self, text, timeout=30, identity=None):
        device = identity or self.args.serial
        console = f"/dev/serial/by-id/usb-zlrs_ZLRS_{device}-if04"
        with serial.Serial(console, 115200, timeout=0.05, write_timeout=2) as port:
            port.write(b"\x03\r")
            time.sleep(0.1)
            port.reset_input_buffer()
            port.write(text.encode() + b"\r")
            output = bytearray()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                output.extend(port.read(4096))
                clean = ANSI.sub("", output.decode(errors="replace"))
                if re.search(r"(?:^|\n)[^\n]*~\$\s*$", clean):
                    with self.log.open("a") as log:
                        log.write(f"\n$ {text}\n{clean}\n")
                    return clean
            raise TimeoutError(f"Console command timed out: {text}: {output!r}")

    def status(self):
        output = self.command("zlrs trace status")
        match = re.search(r"Trace state=(\w+) id=(\d+) requested_ms=(\d+) "
                          r"reason=(\d+) dropped=(\d+) error=(-?\d+)", output)
        slot = re.search(r"generation=(\d+) slot=(-?\d+)", output)
        if match is None or slot is None:
            raise RuntimeError(f"Incomplete capture status: {output}")
        return dict(zip(("state", "id", "requested_ms", "reason", "dropped", "error",
                         "generation", "slot"),
                        (match[1], *map(int, match.groups()[1:]), *map(int, slot.groups()))))

    def reboot(self, identity=None):
        try:
            self.command("kernel reboot cold", timeout=2, identity=identity)
        except (OSError, TimeoutError, serial.SerialException):
            pass

    def management(self, entering):
        if entering:
            self.command("zlrs mgmt")
            time.sleep(0.1)
        else:
            subprocess.run([sys.executable, str(CLI), "exit", "--serial", self.args.serial],
                           check=True, capture_output=True, timeout=15)

    def download(self, source, target, success=True):
        result = subprocess.run([self.smpmgr, "--port", self.console, "--timeout", "5",
                                 "file", "download", source, str(target)],
                                capture_output=True, text=True, timeout=60)
        with (self.output / "smpmgr.log").open("a") as log:
            log.write(f"\nDownload {source}\n{result.stdout}\n{result.stderr}\n")
        if (result.returncode == 0) != success:
            raise RuntimeError(f"Unexpected file download result: {result.stdout} {result.stderr}")

    def export(self, name, complete):
        status = self.status()
        assert status["state"] in ("saved", "idle"), status
        folder = self.output / name
        folder.mkdir(exist_ok=True)
        self.download(f"/trace/{status['slot']}/capture.json", folder / "capture.json")
        self.download(f"/trace/{status['slot']}/channel0_0", folder / "channel0_0")
        shutil.copyfile(self.metadata, folder / "metadata")
        saved = json.loads((folder / "capture.json").read_text())
        raw = (folder / "channel0_0").read_bytes()
        assert len(raw) == saved["capacity"] == 65536
        assert hashlib.sha256(raw).hexdigest() == saved["sha256"]
        definitions, _ = self.viewer.build_event_defs(str(folder / "metadata"))
        trace = self.viewer.parse_trace(str(folder / "channel0_0"), definitions)
        named = [event for event in trace.events if event.name == "named_event"]
        names = [event.fields["name"] for event in named]
        assert "cap_begin" in names, names
        is_complete = "cap_end" in names and saved["core_dropped"] == 0
        if complete is not None:
            assert is_complete == complete, (is_complete, complete)
        assert saved["stopped_ns"] > saved["started_ns"]
        launches = [((event.fields["arg1"] - event.fields["arg0"]) & 0xffffffff)
                    for event in named if event.fields["name"] == "rf_launch"]
        if launches:
            assert max(launches) <= 10, launches
        with (folder / "timeline.txt").open("w") as output:
            subprocess.run([sys.executable, str(self.viewer_path), str(folder / "channel0_0"),
                            "--metadata", str(folder / "metadata"), "--text"],
                           check=True, stdout=output, stderr=subprocess.STDOUT, timeout=15)
        record = {"name": name, "events": len(trace.events), "threads": len(trace.threads),
                  "complete": is_complete, "rf_events": sum(n.startswith("rf_") for n in names),
                  "launch_samples": len(launches), "launch_irq_max_us": max(launches, default=None),
                  "metadata": saved}
        self.result["captures"].append(record)
        print(json.dumps(record), flush=True)
        return saved

    def upload(self, identity):
        with (self.output / "update.log").open("a") as log:
            subprocess.run([sys.executable, str(CLI), "update", "--serial", identity,
                            "--image", str(self.args.image), "--no-wait", "--timeout", "45"],
                           check=True, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        wait_hash(identity, image_hash_of(self.args.image))

    def install(self):
        self.upload(self.args.serial)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            try:
                if self.status()["state"] == "idle":
                    return
            except (OSError, RuntimeError, TimeoutError, serial.SerialException):
                pass
            time.sleep(0.2)
        raise TimeoutError("Trace firmware did not start")

    def storage_failure(self):
        self.command("zlrs trace start 10")
        self.management(True)
        before = self.status()
        inactive = 1 - before["slot"]
        backup = f"/trace/fault-backup-{before['id']}"
        self.command(f"fs mv /trace/{inactive} {backup}")
        try:
            self.command(f"fs write /trace/{inactive} 00")
            self.command("zlrs trace save")
            failed = self.status()
            assert failed["state"] == "frozen" and failed["error"] < 0, failed
            assert failed["generation"] == before["generation"]
        finally:
            self.command(f"fs rm /trace/{inactive}")
            self.command(f"fs mv {backup} /trace/{inactive}")
        self.command("zlrs trace save")
        self.export("storage-retry", complete=True)
        self.management(False)

    def controls(self):
        self.command("zlrs trace start 5001")
        assert self.status()["state"] == "idle"
        self.command("zlrs trace start 1000")
        active = self.status()
        assert active["state"] == "capturing"
        self.command("zlrs trace discard")
        assert self.status()["state"] == "capturing"
        self.command("zlrs trace start 50")
        assert self.status()["id"] == active["id"]
        self.command("zlrs trace stop")
        self.command("zlrs trace stop")
        assert self.status()["reason"] == 2
        self.command("zlrs trace discard")
        assert self.status()["state"] == "idle"
        for duration in (10, 50):
            self.command(f"zlrs trace start {duration}")
            time.sleep(duration / 1000 + 0.03)
            status = self.status()
            assert status["state"] == "frozen" and status["reason"] == 1, status
            self.command("zlrs trace discard")
        assert self.status()["state"] == "idle"
        (self.output / "controls.json").write_text(json.dumps({"passed": True}) + "\n")

    def duplex(self):
        if self.args.peer is None:
            return
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status = self.command("zlrs status")
            if re.search(r"State\s+: connected", status) and re.search(r"Capture\s+: ready", status):
                break
            time.sleep(0.2)
        else:
            raise RuntimeError(f"Peers did not establish DATA capture: {status}")
        with (self.output / "duplex-data.json").open("w") as output:
            process = subprocess.Popen([
                sys.executable, str(REPO / "tests/hardware/app/link/test_data.py"),
                "--leader-port", self.args.serial, "--follower-port", self.args.peer,
                "--rounds", "1", "--bytes", "65537", "--interval-ms", "1",
                "--timeout", "60", "--format", "json"], stdout=output, stderr=subprocess.STDOUT)
            try:
                time.sleep(1.5)
                # Full kernel tracing fills 64 KiB faster under USB/DATA load.
                self.command("zlrs trace start 10")
                assert process.wait(timeout=65) == 0, "Bidirectional DATA failed"
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
        self.command("zlrs trace save")
        self.management(True)
        self.export("duplex", complete=True)
        self.management(False)

    def run(self):
        previous = image_hash(self.args.serial)
        self.result["original_hash"] = previous
        originals = {self.args.serial: previous}
        if self.args.peer is not None:
            originals[self.args.peer] = image_hash(self.args.peer)
        self.result["original_hashes"] = originals
        try:
            if self.args.peer is not None:
                self.upload(self.args.peer)
            self.install()
            self.result["trace_hash"] = image_hash(self.args.serial)
            assert self.result["trace_hash"] != previous
            assert self.status()["state"] == "idle"
            self.controls()
            self.command("zlrs trace start 9")
            assert self.status()["state"] == "idle"
            self.command("zlrs trace start 10")
            time.sleep(0.03)
            assert self.status()["state"] == "frozen"
            self.command("zlrs trace start 100")
            assert self.status()["state"] == "frozen"
            self.command("zlrs trace save")
            assert self.status()["state"] == "saved"
            self.download("/trace/0/channel0_0", self.output / "denied.bin", success=False)
            self.management(True)
            short = self.export("deadline", complete=True)
            self.management(False)

            self.command("zlrs trace start")
            time.sleep(0.1)
            assert self.status()["requested_ms"] == 50
            self.command("zlrs trace save")
            self.management(True)
            self.export("default", complete=True)
            self.management(False)
            self.storage_failure()
            self.duplex()

            self.command("zlrs trace start 5000")
            self.command("zlrs trace stop")
            assert self.status()["reason"] == 2
            self.command("zlrs trace stop")
            self.command("zlrs trace save")
            self.management(True)
            self.export("manual", complete=None)
            self.management(False)

            self.command("zlrs trace start 5000")
            time.sleep(5.1)
            assert self.status()["state"] == "frozen"
            self.command("zlrs trace save")
            self.management(True)
            self.export("capacity", complete=False)
            self.management(False)

            self.command("zlrs trace start 5000")
            self.management(True)
            assert self.status()["reason"] == 3
            self.command("zlrs trace save")
            assert self.status()["state"] == "saved"
            last = self.export("maintenance", complete=None)
            self.management(False)
            self.reboot()
            wait_hash(self.args.serial, previous)
            self.install()
            assert self.status()["state"] == "idle"
            self.management(True)
            restored = self.export("after-reboot", complete=None)
            assert restored["sha256"] == last["sha256"]
            assert restored["generation"] >= short["generation"]
            self.management(False)
            self.result["passed"] = True
        except Exception:
            try:
                self.command("zlrs status")
                self.command("zlrs ports")
                if self.status()["state"] == "frozen":
                    self.command("zlrs trace save")
                    self.management(True)
                    self.export("failure", complete=None)
                    self.management(False)
            except (OSError, RuntimeError, TimeoutError, AssertionError, subprocess.SubprocessError):
                pass
            raise
        finally:
            try:
                for identity, original in originals.items():
                    if image_hash(identity) != original:
                        self.reboot(identity)
                    wait_hash(identity, original)
                self.result["restored"] = True
            finally:
                (self.output / "summary.json").write_text(json.dumps(self.result, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--peer", help="Follower serial for optional bidirectional DATA acceptance")
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--zephyr", type=Path, default=REPO.parent / "zephyr")
    args = parser.parse_args()
    Acceptance(args).run()


if __name__ == "__main__":
    main()
