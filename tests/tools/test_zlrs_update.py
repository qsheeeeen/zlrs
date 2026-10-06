# SPDX-License-Identifier: Apache-2.0
import contextlib
import io
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch

from support import image_bytes, invoke
import zlrs_update as update
import zlrs_recovery as recovery


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
    def test_port_patterns_separate_application_and_recovery(self):
        ports = ["/dev/serial/by-id/usb-zlrs_ZLRS_1234-if01",
                 "/dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_1234-if01"]
        with patch.object(update.glob, "glob", return_value=ports):
            self.assertEqual(update.find_by_id(update.APP_PORT_HINT,
                             exclude=update.RECOVERY_PORT_HINT), [ports[0]])
            self.assertEqual(update.find_by_id(update.RECOVERY_PORT_HINT), [ports[1]])

    def test_upload_command_targets_inactive_slot_and_leaves_confirmation_to_host(self):
        image = Path("zephyr.signed.bin")
        command = update.upload_command("smpmgr", "/dev/ttyACM0", image, update.DEFAULT_BAUD)
        self.assertEqual(command[0], "smpmgr")
        self.assertIn("upgrade", command)
        self.assertEqual(command[-1], str(image))
        self.assertNotIn("--confirm", command)
        command = update.slot_upload_command("smpmgr", "/dev/ttyACM0", image,
                                             update.DEFAULT_BAUD, 1)
        self.assertIn("--slot", command)
        self.assertEqual(command[command.index("--slot") + 1], "2")

    def test_management_probe_and_trigger(self):
        command = update.mgmt_probe_command("/dev/ttyACM0")
        self.assertIn("status", command)
        self.assertIn("/dev/ttyACM0", command)
        self.assertEqual(recovery.build_request(), bytes.fromhex("a55a034100e4"))
