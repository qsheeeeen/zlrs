#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise shell-free bounded captures with MCUmgr and Zephyr's viewer."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

import serial

REPO = Path(__file__).resolve().parents[4]
CLI = REPO / "tools/zlrs.py"
sys.path.insert(0, str(REPO / "tests/app"))
from unit.hardware.run import image_hash
sys.path.insert(0, str(REPO / "tools"))
from zlrs_update import image_hash_of
from zlrs_smp import request as smp_request
from zlrs_trace import command_fields, OPERATIONS, STATES
from zlrs_mgmt import GROUP, ManagementClient, CMD_STATUS, STATUS_VIEWS


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
        self.smp_port = f"/dev/serial/by-id/usb-zlrs_ZLRS_{args.serial}-if02"
        self.output = args.out
        self.output.mkdir(parents=True, exist_ok=True)
        self.log = self.output / "control.jsonl"
        self.result = {"serial": args.serial, "captures": []}
        self.smpmgr = str(Path(sys.executable).with_name("smpmgr"))
        self.metadata = args.zephyr / "subsys/tracing/ctf/tsdl/metadata"
        self.viewer_path = args.zephyr / "scripts/tracing/trace_viewer.py"
        spec = importlib.util.spec_from_file_location("zephyr_trace_viewer", self.viewer_path)
        self.viewer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.viewer)

    def control(self, action, duration=None, success=True):
        fields = command_fields(OPERATIONS.get(action), duration if duration is not None else 50)
        response = smp_request(self.smp_port, GROUP, fields)
        with self.log.open("a") as log:
            log.write(json.dumps({"action": action, "fields": {"op": fields["op"], "value": fields["value"].hex()}, "response": response}) + "\n")
        assert (response["result"] == 0) == success, response
        response["state"] = STATES[response["state"]]
        return response

    def status(self):
        return self.control("status")

    def reboot(self, identity=None):
        port = f"/dev/serial/by-id/usb-zlrs_ZLRS_{identity or self.args.serial}-if02"
        subprocess.run([self.smpmgr, "--port", port, "os", "reset"],
                       check=True, capture_output=True, timeout=15)

    def device_status(self):
        with ManagementClient(self.smp_port, timeout=5) as client:
            return client.request(CMD_STATUS)["status"]

    def management(self, entering):
        if entering:
            subprocess.run([sys.executable, str(CLI), "enter", "--serial", self.args.serial],
                           check=True, capture_output=True, timeout=15)
            time.sleep(0.1)
        else:
            subprocess.run([sys.executable, str(CLI), "exit", "--serial", self.args.serial],
                           check=True, capture_output=True, timeout=15)

    def download(self, source, target, success=True):
        result = subprocess.run([self.smpmgr, "--port", self.smp_port, "--timeout", "5",
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
        filtered = [event.name for event in trace.events
                    if event.name.startswith(("gpio_port_", "gpio_pin_"))
                    or "busy_wait" in event.name]
        assert not filtered, f"Filtered API events reached the CTF stream: {filtered[:5]}"
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

    def controls(self):
        self.control('start', 5001, success=False)
        assert self.status()["state"] == "idle"
        self.control('start', 1000)
        active = self.status()
        assert active["state"] == "capturing"
        self.control("discard", success=False)
        assert self.status()["state"] == "capturing"
        self.control("start", 50, success=False)
        assert self.status()["id"] == active["id"]
        self.control('stop')
        self.control('stop')
        assert self.status()["reason"] == 2
        self.control('discard')
        assert self.status()["state"] == "idle"
        for duration in (10, 50):
            self.control("start", duration)
            time.sleep(duration / 1000 + 0.03)
            status = self.status()
            assert status["state"] == "frozen" and status["reason"] == 1, status
            self.control('discard')
        assert self.status()["state"] == "idle"
        (self.output / "controls.json").write_text(json.dumps({"passed": True}) + "\n")

    def duplex(self):
        if self.args.peer is None:
            return
        before = self.radio_late_counts("duplex-before")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status = self.device_status()
            if status["state"] == 4 and status["flags"] & 4 and not status["flags"] & 32:
                break
            time.sleep(0.2)
        else:
            raise RuntimeError(f"Peers did not establish DATA capture: {status}")
        with (self.output / "duplex-data.json").open("w") as output:
            process = subprocess.Popen([
                sys.executable, str(REPO / "tests/app/link/hardware/test_data.py"),
                "--leader-port", self.args.serial, "--follower-port", self.args.peer,
                "--rounds", "1", "--bytes", "65537", "--interval-ms", "1",
                "--timeout", "60", "--format", "json"], stdout=output, stderr=subprocess.STDOUT)
            try:
                time.sleep(1.5)
                self.control('start', 50)
                assert process.wait(timeout=65) == 0, "Bidirectional DATA failed"
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
        after = self.radio_late_counts("duplex-after")
        assert after == before, f"TX deadlines missed during DATA/capture: {before} -> {after}"
        self.control('save')
        self.management(True)
        self.export("duplex", complete=True)
        self.management(False)

    def radio_late_counts(self, name):
        with ManagementClient(self.smp_port, 115200, 5) as client:
            report = client.request(CMD_STATUS, bytes([STATUS_VIEWS["radio"]]))["status"]
        (self.output / f"{name}-radio.json").write_text(json.dumps(report, indent=2, default=bytes.hex) + "\n")
        return report["prepare_late_starts"], report["trigger_late_starts"]

    def restore(self, originals):
        errors = {}
        for identity, original in originals.items():
            try:
                if image_hash(identity) != original:
                    self.reboot(identity)
            except Exception as error:
                errors.setdefault(identity, {})["request"] = str(error)
        restored = True
        for identity, original in originals.items():
            try:
                wait_hash(identity, original)
            except Exception as error:
                restored = False
                errors.setdefault(identity, {})["verify"] = str(error)
        self.result["restored"] = restored
        if errors:
            self.result["restore_errors"] = errors
        if not restored:
            raise RuntimeError(f"Firmware restoration failed: {errors}")

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
            self.control('start', 9, success=False)
            assert self.status()["state"] == "idle"
            self.control('start', 10)
            time.sleep(0.03)
            assert self.status()["state"] == "frozen"
            self.control('start', 100, success=False)
            assert self.status()["state"] == "frozen"
            self.control('save')
            assert self.status()["state"] == "saved"
            self.download("/trace/0/channel0_0", self.output / "denied.bin", success=False)
            self.management(True)
            short = self.export("deadline", complete=True)
            self.management(False)

            self.control('start')
            time.sleep(0.1)
            assert self.status()["requested_ms"] == 50
            self.control('save')
            self.management(True)
            self.export("default", complete=True)
            self.management(False)
            self.duplex()

            self.control('start', 5000)
            self.control('stop')
            assert self.status()["reason"] == 2
            self.control('stop')
            self.control('save')
            self.management(True)
            self.export("manual", complete=None)
            self.management(False)

            self.control('start', 5000)
            time.sleep(5.1)
            assert self.status()["state"] == "frozen"
            self.control('save')
            self.management(True)
            self.export("capacity", complete=False)
            self.management(False)

            self.control('start', 5000)
            self.management(True)
            assert self.status()["reason"] == 3
            self.control('save')
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
                self.device_status()
                if self.status()["state"] == "frozen":
                    self.control('save')
                    self.management(True)
                    self.export("failure", complete=None)
                    self.management(False)
            except (OSError, RuntimeError, TimeoutError, AssertionError, subprocess.SubprocessError):
                pass
            raise
        finally:
            try:
                self.restore(originals)
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
