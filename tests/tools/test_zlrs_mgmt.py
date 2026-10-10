# SPDX-License-Identifier: Apache-2.0
import argparse
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

from support import invoke
import zlrs_mgmt as mgmt


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
                 ("set-key-hex", "z" * 64),
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

    def test_log_read_ack_reports_status_and_offset_then_exits(self):
        cases = ((bytes([mgmt.CMD_LOG_READ, 2]), "wrong state"),
                 (bytes([mgmt.CMD_LOG_READ, 0]), "received only an OK ACK"),
                 (bytes([mgmt.CMD_STATUS, 2]), "mismatched ACK"),
                 (b"", "malformed"))
        for ack, reason in cases:
            with self.subTest(reason=reason):
                link = Mock()
                link.__enter__ = Mock(return_value=link)
                link.__exit__ = Mock(return_value=False)
                info = b"\x01\x00\x11\x00" + (160).to_bytes(4, "little")
                chunk = b"\x00" + bytes(4) + b"z" * 80
                responses = io.BytesIO(
                    mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_ENTER, 0])) +
                    mgmt.encode(mgmt.LOG_INFO_RESP, info) +
                    mgmt.encode(mgmt.LOG_DATA_RESP, chunk) +
                    mgmt.encode(mgmt.ACK, ack) +
                    mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_EXIT, 0])))
                link.read.side_effect = responses.read
                output = io.StringIO()
                with tempfile.TemporaryDirectory() as directory, \
                        patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}), \
                        contextlib.redirect_stdout(io.StringIO()), \
                        contextlib.redirect_stderr(output):
                    self.assertEqual(mgmt.dump_logs("fake", 115200, 1, directory), 1)
                    self.assertEqual((Path(directory) / "log.0017.bin").read_bytes(), b"z" * 80)
                self.assertIn("offset 80", output.getvalue())
                self.assertIn(reason, output.getvalue())
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

class FramingTests(unittest.TestCase):
    def test_firmware_frame_vectors(self):
        cases = [
            ("enter", mgmt.CMD_ENTER, b"", "a55a04000073"),
            ("status", mgmt.CMD_STATUS, b"", "a55a04010078"),
            ("pair", mgmt.CMD_PAIR, b"", "a55a04020065"),
            ("erase-pair", mgmt.CMD_ERASE_PAIR, b"", "a55a0403006e"),
            ("set-phrase", mgmt.CMD_SET_PHRASE, b"hello", "a55a04050568656c6c6f78"),
            ("set-key-hex", mgmt.CMD_SET_KEY_HEX, b"00" * 32, None),
            ("set-baud", mgmt.CMD_SET_BAUD, bytes([0x00, 0x25, 0x06, 0x00]), "a55a040804002506004e"),
            ("set-region", mgmt.CMD_SET_REGION, bytes([3]), "a55a0409010305"),
            ("set-power", mgmt.CMD_SET_POWER, bytes([20]), "a55a040a011453"),
            ("set-role", mgmt.CMD_SET_ROLE, bytes([1]), "a55a040b0101a9"),
            ("set-profile", mgmt.CMD_SET_PROFILE, bytes([2]), "a55a040c010220"),
            ("new-dev-id", mgmt.CMD_NEW_DEV_ID, b"", "a55a040e0011"),
            ("factory-reset", mgmt.CMD_FACTORY_RESET, b"", "a55a041000c3"),
            ("recovery", mgmt.CMD_RECOVERY, b"", "a55a041100c8"),
            ("build", mgmt.CMD_BUILD, b"", "a55a04060049"),
            ("image-hash", mgmt.CMD_IMAGE_HASH, b"", "a55a04070042"),
            ("confirm", mgmt.CMD_CONFIRM, b"", "a55a041200d5"),
            ("exit", mgmt.CMD_EXIT, b"", "a55a040f001a"),
        ]
        for name, command, payload, expected in cases:
            with self.subTest(command=name):
                frame = mgmt.encode(command, payload)
                self.assertEqual(mgmt.decode(frame), (command, payload))
                if expected is not None:
                    self.assertEqual(frame.hex(), expected)

    def test_known_frame_decode(self):
        self.assertEqual(mgmt.decode(bytes.fromhex("a55a04050568656c6c6f78")),
                         (mgmt.CMD_SET_PHRASE, b"hello"))


    def test_single_module_is_a_target_list(self):
        args = argparse.Namespace(port=None, serial=None, all=False)
        with patch.object(mgmt, "app_ports", return_value=["/dev/serial/by-id/usb-fake-if02"]):
            self.assertEqual(mgmt.pick_targets(args, argparse.ArgumentParser()),
                             ["/dev/serial/by-id/usb-fake-if02"])
