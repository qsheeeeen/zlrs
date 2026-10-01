#!/usr/bin/env python3
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
"""Operator workflow regressions. All ports and upload processes are simulated."""

import argparse
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


from zlrs_cli import fields, styled

mgmt = load("zlrs_mgmt")
update = load("zlrs_update")
entry = load("zlrs")


def invoke(module, *args):
    output = io.StringIO()
    with patch.object(sys, "argv", [module.__file__, *args]), \
            contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        try:
            rc = module.main()
        except SystemExit as exc:
            rc = exc.code
    return rc, output.getvalue()


def image_bytes(protected=False):
    body = b"test firmware"
    protected_data = struct.pack("<HH", 0x6908, 4) if protected else b""
    header = struct.pack("<IIHHI", 0x96F3B83D, 0, 32, len(protected_data), len(body))
    content = header.ljust(32, b"\0") + body + protected_data
    digest = hashlib.sha256(content).digest()
    return content + struct.pack("<HHHH", 0x6907, 40, 0x10, 32) + digest


class EntryTests(unittest.TestCase):
    def test_help_lists_all_tools_and_excludes_bringup(self):
        rc, output = invoke(entry, "--help")
        self.assertEqual(rc, 0)
        for name in ("status", "set-role", "list", "update", "recovery", "throughput"):
            self.assertIn(name, output)
        self.assertNotIn("bringup", output)
        self.assertNotIn("--ui", output)

    def test_removed_bringup_is_rejected(self):
        self.assertEqual(invoke(entry, "bringup")[0], 2)
        self.assertFalse((TOOLS / "zlrs-bringup.py").exists())

    def test_management_routing_and_data_baud(self):
        with patch.object(entry.mgmt, "talk", return_value=0) as talk:
            rc, _ = invoke(entry, "set-baud", "420000", "--port", "fake", "--baud", "115200")
        self.assertEqual(rc, 0)
        self.assertEqual(talk.call_args_list[0].args[1], 115200)
        self.assertEqual(entry.mgmt.decode(talk.call_args_list[0].args[2]),
                         (entry.mgmt.CMD_SET_BAUD, (420000).to_bytes(4, "little")))

    def test_data24_mode_command(self):
        with patch.object(entry.mgmt, "talk", return_value=0) as talk:
            rc, _ = invoke(entry, "set-mode", "data24", "--port", "fake")
        self.assertEqual(rc, 0)
        self.assertEqual(entry.mgmt.decode(talk.call_args_list[0].args[2]),
                         (entry.mgmt.CMD_SET_MODE, b"\x01"))
        self.assertEqual(invoke(entry, "set-mode", "invalid")[0], 2)
        status = bytearray(21)
        status[3] = 0x08
        self.assertIn("data24 (2.4 GHz data only)", entry.mgmt.status_text(status))

    def test_subcommand_help_uses_public_name(self):
        for command in ("status", "update", "recovery", "throughput"):
            with self.subTest(command=command):
                rc, output = invoke(entry, command, "--help")
                self.assertEqual(rc, 0)
                self.assertIn("zlrs.py " + command, output)

    def test_list_does_not_open_serial_port(self):
        with patch.object(entry.mgmt, "app_ports", return_value=["fake-if04"]), \
                patch.object(entry.mgmt, "talk") as talk:
            rc, output = invoke(entry, "list")
        self.assertEqual(rc, 0)
        self.assertIn("fake-if04", output)
        talk.assert_not_called()

    def test_throughput_dispatches_parsed_arguments(self):
        with patch.object(entry.throughput, "main", return_value=0) as measure:
            rc, _ = invoke(entry, "throughput", "--tx-port", "tx", "--rx-port", "rx")
        self.assertEqual(rc, 0)
        self.assertEqual(measure.call_args.kwargs["args"].tx_port, "tx")
        self.assertEqual(measure.call_args.kwargs["args"].rx_port, "rx")

    def test_all_self_tests_are_run_without_hardware(self):
        with patch.object(entry.mgmt, "self_test", return_value=0) as management, \
                patch.object(entry.recovery, "self_test", return_value=0) as recovery, \
                patch.object(entry.update, "self_test", return_value=1) as update_test:
            rc, _ = invoke(entry, "--self-test")
        self.assertEqual(rc, 1)
        management.assert_called_once()
        recovery.assert_called_once()
        update_test.assert_called_once()

    def test_update_helpers_call_the_public_entry(self):
        probe = update.mgmt_subcmd_command("fake", "image-hash")
        self.assertEqual(Path(probe[1]).name, "zlrs.py")
        self.assertEqual(probe[2], "image-hash")
        with patch.object(update.subprocess, "run", return_value=Mock(returncode=0)) as trigger, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(update.run_trigger("fake", 115200), 0)
        command = trigger.call_args.args[0]
        self.assertEqual(Path(command[1]).name, "zlrs.py")
        self.assertEqual(command[2], "recovery")


class ManagementTests(unittest.TestCase):
    def test_data_baud_does_not_change_connection_baud(self):
        with patch.object(mgmt, "talk", return_value=0) as talk:
            rc, _ = invoke(mgmt, "--port", "fake", "--baud", "115200", "set-baud", "420000")
        self.assertEqual(rc, 0)
        self.assertEqual(talk.call_args_list[0].args[1], 115200)
        self.assertEqual(mgmt.decode(talk.call_args_list[0].args[2]),
                         (mgmt.CMD_SET_BAUD, (420000).to_bytes(4, "little")))

    def test_invalid_parameters_fail_before_port_access(self):
        cases = [("set-phrase", ""), ("set-phrase", "中" * 11),
                 ("set-key-hex", "z" * 64), ("set-failsafe", "bad"),
                 ("set-baud", "0"), ("set-power", "256")]
        with patch.object(mgmt, "talk") as talk:
            for command, value in cases:
                with self.subTest(command=command, value=value):
                    rc, output = invoke(mgmt, "--dry-run", command, value)
                    self.assertEqual(rc, 2)
                    self.assertNotIn("Traceback", output)
            talk.assert_not_called()

    def test_failure_still_exits_management(self):
        with patch.object(mgmt, "talk", side_effect=[1, 0]) as talk:
            rc, _ = invoke(mgmt, "--port", "fake", "status")
        self.assertEqual(rc, 1)
        self.assertEqual(mgmt.decode(talk.call_args_list[1].args[2])[0], mgmt.CMD_EXIT)

    def test_exit_failure_is_reported(self):
        with patch.object(mgmt, "talk", side_effect=[0, 1]):
            rc, output = invoke(mgmt, "--port", "fake", "status")
        self.assertEqual(rc, 1)
        self.assertIn("restore the radio", output)

    def test_dry_run_obeys_session_flags(self):
        rc, output = invoke(mgmt, "--dry-run", "--no-enter", "--keep", "status")
        self.assertEqual(rc, 0)
        self.assertNotIn("enter:", output)
        self.assertNotIn("exit :", output)

    def test_serial_substring_must_be_unique(self):
        args = argparse.Namespace(port=None, serial="abc", all=False)
        parser = argparse.ArgumentParser()
        with patch.object(mgmt, "app_ports", return_value=["ABC1", "ABC2"]), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            mgmt.pick_targets(args, parser)

    def test_frame_resynchronizes_overlapping_header(self):
        stream = io.BytesIO(b"noise\xa5" + mgmt.encode(mgmt.ACK, b"\x00\x00"))
        self.assertEqual(mgmt.read_response(stream), (mgmt.ACK, b"\x00\x00"))


    def test_log_dry_run_does_not_open_ports_or_create_directory(self):
        with patch.object(mgmt, "dump_logs") as dump:
            rc, output = invoke(mgmt, "--dry-run", "logs", "/unused/path")
        self.assertEqual(rc, 0)
        self.assertIn("no files or ports opened", output)
        dump.assert_not_called()

    def test_malformed_log_ack_still_exits_session(self):
        link = Mock()
        link.__enter__ = Mock(return_value=link)
        link.__exit__ = Mock(return_value=False)
        responses = io.BytesIO(mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_ENTER, 0])) +
                               mgmt.encode(mgmt.ACK, b"") +
                               mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_EXIT, 0])))
        link.read.side_effect = responses.read
        with tempfile.TemporaryDirectory() as directory, \
                patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}), \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(mgmt.dump_logs("fake", 115200, 1, directory), 1)
        self.assertEqual(link.write.call_args.args[0], mgmt.encode(mgmt.CMD_EXIT))

    def test_help_has_no_interactive_option(self):
        rc, output = invoke(mgmt, "--help")
        self.assertEqual(rc, 0)
        self.assertNotIn("--ui", output)
        self.assertIn("commands:", output)

    def test_status_groups_fields_and_formats_units(self):
        payload = bytearray(21)
        payload[1] = 1
        payload[2] = 4
        payload[3] = 7
        payload[12] = 87
        payload[14] = 2
        payload[15] = 246  # signed -10 dBm
        payload[16:20] = (420000).to_bytes(4, "little")
        payload[20] = 1
        output = mgmt.status_text(payload)
        for text in ("Device\n", "Link\n", "Configuration\n", "connected", "87%",
                     "CN470", "-10 dBm", "420000 baud", "Root key   : configured"):
            self.assertIn(text, output)
        self.assertNotIn("\033[", output)

    def test_image_hash_stays_a_plain_value(self):
        digest = bytes(range(32))
        link = Mock()
        link.__enter__ = Mock(return_value=link)
        link.__exit__ = Mock(return_value=False)
        response = io.BytesIO(mgmt.encode(mgmt.IMAGE_HASH_RESP, digest))
        link.read.side_effect = response.read
        output = io.StringIO()
        with patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}), \
                contextlib.redirect_stdout(output):
            rc = mgmt.talk("fake", 115200, mgmt.encode(mgmt.CMD_IMAGE_HASH), True, 1, None)
        self.assertEqual(rc, 0)
        self.assertEqual(output.getvalue(), digest.hex() + "\n")


class OutputTests(unittest.TestCase):
    def test_fields_align_label_separator(self):
        output = fields([("Device", "1234"), ("Size", "1,024 bytes")])
        lines = output.splitlines()
        self.assertEqual(lines[0].index(":"), lines[1].index(":"))

    def test_color_respects_redirects_and_no_color(self):
        stream = Mock()
        with patch.dict(os.environ, {"TERM": "xterm"}, clear=True):
            stream.isatty.return_value = False
            self.assertEqual(styled("ok", "32", stream), "ok")
            stream.isatty.return_value = True
            self.assertEqual(styled("ok", "32", stream), "\033[32mok\033[0m")
            with patch.dict(os.environ, {"NO_COLOR": ""}):
                self.assertEqual(styled("ok", "32", stream), "ok")


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.image = Path(self.tmp.name) / "signed image.bin"
        self.image.write_bytes(image_bytes())
        self.options = ["--serial", "1234", "--image", str(self.image)]

    def test_hash_parsing_and_corruption(self):
        for protected in (False, True):
            data = image_bytes(protected)
            self.image.write_bytes(data)
            self.assertEqual(update.image_hash_of(self.image), data[-32:].hex())
            self.image.write_bytes(data[:32] + b"X" + data[33:])
            self.assertIsNone(update.image_hash_of(self.image))
        self.image.write_bytes(b"junk" + struct.pack("<HH", 0x10, 32) + bytes(32))
        self.assertIsNone(update.image_hash_of(self.image))

    def test_invalid_image_does_not_trigger_reboot(self):
        self.image.write_bytes(b"invalid")
        with patch.object(update, "run_trigger") as trigger:
            rc, _ = invoke(update, *self.options)
            self.assertEqual(rc, 2)
            trigger.assert_not_called()

    def test_missing_client_does_not_trigger_reboot(self):
        with patch.object(update, "find_client", return_value=None), \
                patch.object(update, "run_trigger") as trigger:
            rc, _ = invoke(update, *self.options)
            self.assertEqual(rc, 2)
            trigger.assert_not_called()

    def test_dry_run_does_not_run_external_commands(self):
        with patch.object(update.subprocess, "run") as external:
            rc, output = invoke(update, *self.options, "--dry-run")
            self.assertEqual(rc, 0)
            self.assertIn("[dry-run]", output)
            external.assert_not_called()

    def test_build_directory_environment(self):
        with patch.dict(os.environ, {"ZLRS_WS": "/workspace", "ZLRS_BUILD_DIR": "build-lf"}):
            self.assertEqual(update.default_image(),
                             Path("/workspace/build-lf/app/zephyr/zephyr.signed.bin"))

    def test_usb_serial_survives_modes(self):
        app = "/dev/serial/by-id/usb-zlrs_ZLRS_AbCd-if04"
        recovery = "/dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_AbCd-if00"
        with patch.object(update, "find_by_id", return_value=[]):
            self.assertEqual(update.serial_of_port(app), "AbCd")
            self.assertEqual(update.serial_of_port(recovery), "AbCd")

    def test_wait_uses_management_and_recovery_cdc_only(self):
        for hint, ports, wanted in [
            (update.APP_PORT_HINT, ["app-if00", "app-if02", "app-if04"], "app-if04"),
            (update.RECOVERY_PORT_HINT, ["boot-if00", "boot-if02"], "boot-if00"),
        ]:
            with patch.object(update, "find_by_id", return_value=ports):
                self.assertEqual(update.wait_for_by_id(hint, 1), wanted)
        with patch.object(update, "find_by_id", return_value=["a-if00", "b-if00"]):
            with self.assertRaises(ValueError):
                update.wait_for_by_id(update.RECOVERY_PORT_HINT, 1)

    def test_exact_serial_does_not_follow_a_substring_match(self):
        selected = "/dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_1234-if00"
        other = "/dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_12345-if00"
        with patch.object(update, "find_by_id", return_value=[other, selected]):
            self.assertEqual(update.wait_for_by_id(update.RECOVERY_PORT_HINT, 1,
                             serial="1234", exact_serial=True), selected)

    def test_failed_slot_query_stops(self):
        with patch.object(update.subprocess, "run", return_value=Mock(
                returncode=1, stdout="slot=0\nactive=True", stderr="failed")):
            with self.assertRaises(ValueError):
                update.inactive_slot("smpmgr", "fake", 115200)

    def test_verification_precedes_confirmation(self):
        for matching, no_confirm in [(True, False), (False, False), (False, True), (True, True)]:
            with self.subTest(matching=matching, no_confirm=no_confirm):
                digest = self.image.read_bytes()[-32:].hex() if matching else "0" * 64
                with patch.object(update, "find_client", return_value="smpmgr"), \
                        patch.object(update, "inactive_slot", return_value=1), \
                        patch.object(update, "wait_for_by_id", return_value="app-if04"), \
                        patch.object(update.subprocess, "run", return_value=Mock(returncode=0)), \
                        patch.object(update, "run_mgmt_subcmd_retry", side_effect=[(0, digest), (0, "ok")]) as mgmt_call:
                    options = self.options + ["--no-trigger", "--recovery-port", "fake"]
                    if no_confirm:
                        options.append("--no-confirm-check")
                    rc, _ = invoke(update, *options)
                self.assertEqual(rc, 0 if matching else 1)
                self.assertEqual(mgmt_call.call_count, 2 if matching and not no_confirm else 1)

    def test_manual_upload_and_no_wait_are_partial_success(self):
        for mode in ("manual", "no-wait"):
            with patch.object(update, "find_client", return_value="smpmgr"), \
                    patch.object(update, "inactive_slot", return_value=1), \
                    patch.object(update.subprocess, "run", return_value=Mock(returncode=0)) as upload, \
                    patch.object(update, "wait_for_by_id") as wait:
                options = self.options + ["--no-trigger", "--recovery-port", "fake"]
                options += ["--client", "none"] if mode == "manual" else ["--no-wait"]
                rc, output = invoke(update, *options)
                self.assertEqual(rc, 0)
                self.assertIn("confirmed" if mode == "manual" else "confirmation", output)
                wait.assert_not_called()
                self.assertEqual(upload.call_count, 0 if mode == "manual" else 1)


if __name__ == "__main__":
    unittest.main()
