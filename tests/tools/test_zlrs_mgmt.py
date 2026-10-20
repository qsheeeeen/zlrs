# SPDX-License-Identifier: Apache-2.0
import argparse
import contextlib
import io
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

from support import REPO, invoke
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
                 ("set-baud", "0")]
        with patch.object(mgmt, "talk") as talk:
            for command, value in cases:
                with self.subTest(command=command, value=value):
                    rc, output = invoke(mgmt, "--dry-run", command, value)
                    self.assertEqual(rc, 2)
                    self.assertNotIn("Traceback", output)
            talk.assert_not_called()

    def test_failure_still_exits_management(self):
        link = Mock()
        link.__enter__ = Mock(return_value=link)
        link.__exit__ = Mock(return_value=False)
        responses = io.BytesIO(mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_OBSERVE, 2])) +
                               mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_ENTER, 0])) +
                               mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_STATUS, 2])) +
                               mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_EXIT, 0])))
        link.read.side_effect = responses.read
        with patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}):
            rc, _ = invoke(mgmt, "--port", "fake", "status")
        self.assertEqual(rc, 1)
        self.assertEqual([mgmt.decode(call.args[0])[0] for call in link.write.call_args_list],
                         [mgmt.CMD_OBSERVE, mgmt.CMD_ENTER, mgmt.CMD_STATUS, mgmt.CMD_EXIT])

    def test_pair_busy_is_reported_and_management_exits(self):
        link = Mock()
        link.__enter__ = Mock(return_value=link)
        link.__exit__ = Mock(return_value=False)
        responses = io.BytesIO(mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_ENTER, 0])) +
                               mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_PAIR, 5])) +
                               mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_EXIT, 0])))
        link.read.side_effect = responses.read
        with patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}):
            rc, output = invoke(mgmt, "--port", "fake", "pair")
        self.assertEqual(rc, 1)
        self.assertIn("temporarily unavailable", output)
        self.assertEqual([mgmt.decode(call.args[0])[0] for call in link.write.call_args_list],
                         [mgmt.CMD_ENTER, mgmt.CMD_PAIR, mgmt.CMD_EXIT])

    def test_exit_failure_is_reported(self):
        link = Mock()
        link.__enter__ = Mock(return_value=link)
        link.__exit__ = Mock(return_value=False)
        responses = io.BytesIO(mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_ENTER, 0])) +
                               mgmt.encode(mgmt.BUILD_RESP, b"test") +
                               mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_EXIT, 2])))
        link.read.side_effect = responses.read
        with patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}):
            rc, output = invoke(mgmt, "--port", "fake", "--refresh", "build")
        self.assertEqual(rc, 1)
        self.assertIn("restore the radio", output)

    def test_dry_run_obeys_session_flags(self):
        rc, output = invoke(mgmt, "--dry-run", "--no-enter", "--keep", "status")
        self.assertEqual(rc, 0)
        self.assertNotIn("enter:", output)
        self.assertNotIn("exit :", output)

    def test_session_flags_and_one_connection(self):
        for options, commands in [
            ([], [mgmt.CMD_BUILD]),
            (["--refresh"], [mgmt.CMD_ENTER, mgmt.CMD_BUILD, mgmt.CMD_EXIT]),
            (["--keep"], [mgmt.CMD_ENTER, mgmt.CMD_BUILD]),
            (["--no-enter"], [mgmt.CMD_BUILD, mgmt.CMD_EXIT]),
        ]:
            with self.subTest(options=options):
                link = Mock()
                link.__enter__ = Mock(return_value=link)
                link.__exit__ = Mock(return_value=False)
                responses = io.BytesIO(b"".join(
                    mgmt.encode(mgmt.BUILD_RESP, b"test") if cmd == mgmt.CMD_BUILD else
                    mgmt.encode(mgmt.ACK, bytes([cmd, 0])) for cmd in commands))
                link.read.side_effect = responses.read
                factory = Mock(return_value=link)
                with patch.dict(sys.modules, {"serial": Mock(Serial=factory)}):
                    rc, _ = invoke(mgmt, "--port", "fake", *options, "build")
                self.assertEqual(rc, 0)
                factory.assert_called_once()
                self.assertEqual([mgmt.decode(call.args[0])[0]
                                  for call in link.write.call_args_list], commands)

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

    def test_online_status_has_timestamps_and_never_enters_management(self):
        link = Mock()
        link.__enter__ = Mock(return_value=link)
        link.__exit__ = Mock(return_value=False)
        snapshot = (16).to_bytes(4, "little") + (0xfffffff0).to_bytes(4, "little") + bytes(mgmt.STATUS_LEN)
        responses = io.BytesIO(mgmt.encode(mgmt.OBSERVE_RESP, snapshot))
        link.read.side_effect = responses.read
        with patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}):
            rc, output = invoke(mgmt, "--port", "fake", "status")
        self.assertEqual(rc, 0)
        self.assertIn("32 ms", output)
        self.assertEqual([mgmt.decode(call.args[0])[0] for call in link.write.call_args_list],
                         [mgmt.CMD_OBSERVE])

    def test_online_query_falls_back_only_when_unsupported(self):
        for status in (2, 4, 3):
            with self.subTest(status=status):
                link = Mock()
                link.__enter__ = Mock(return_value=link)
                link.__exit__ = Mock(return_value=False)
                responses = io.BytesIO(
                    mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_BUILD, status])) +
                    mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_ENTER, 0])) +
                    mgmt.encode(mgmt.BUILD_RESP, b"legacy") +
                    mgmt.encode(mgmt.ACK, bytes([mgmt.CMD_EXIT, 0])))
                link.read.side_effect = responses.read
                with patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}):
                    rc, output = invoke(mgmt, "--port", "fake", "build")
                expected = [mgmt.CMD_BUILD]
                if status in (2, 4):
                    expected += [mgmt.CMD_ENTER, mgmt.CMD_BUILD, mgmt.CMD_EXIT]
                    self.assertEqual(rc, 0)
                    self.assertIn("legacy", output)
                else:
                    self.assertEqual(rc, 1)
                self.assertEqual([mgmt.decode(call.args[0])[0] for call in link.write.call_args_list],
                                 expected)

    def test_malformed_observation_does_not_change_management_mode(self):
        link = Mock()
        link.__enter__ = Mock(return_value=link)
        link.__exit__ = Mock(return_value=False)
        responses = io.BytesIO(mgmt.encode(mgmt.OBSERVE_RESP, bytes(mgmt.STATUS_LEN)))
        link.read.side_effect = responses.read
        with patch.dict(sys.modules, {"serial": Mock(Serial=Mock(return_value=link))}):
            rc, output = invoke(mgmt, "--port", "fake", "status")
        self.assertEqual(rc, 1)
        self.assertIn("snapshot length", output)
        self.assertEqual(link.write.call_count, 1)

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
        payload = bytearray(mgmt.STATUS_LEN)
        payload[1] = 1
        payload[2] = 4
        payload[3] = 0x87
        payload[12] = 87
        payload[14] = 2
        payload[15] = 246  # signed -10 dBm
        payload[16:20] = (420000).to_bytes(4, "little")
        payload[20] = 1
        output = mgmt.status_text(payload)
        for text in ("Link\n", "Radio\n", "Signal / Power\n", "Traffic / Errors\n",
                     "Device / Configuration\n", "Sensors\n", "LR2021 snapshot\n",
                     "connected", "87%", "CN470", "-10 dBm", "420000 baud"):
            self.assertIn(text, output)
        self.assertRegex(output, r"Root key\s*:\s*configured")
        self.assertRegex(output, r"Power control\s*:\s*debug fixed")
        self.assertNotIn("\033[", output)

    def test_status_signed_measurements_and_counters(self):
        payload = bytearray(mgmt.STATUS_LEN)
        payload[21:23] = (-48).to_bytes(2, "little", signed=True)
        payload[23:25] = (-51).to_bytes(2, "little", signed=True)
        payload[25] = 97
        payload[26] = 7
        payload[28:30] = (-125).to_bytes(2, "little", signed=True)
        for offset, value in zip(range(30, 62, 4), (1024, 1000, 24, 1, 3, 0, 2, 4)):
            payload[offset:offset + 4] = value.to_bytes(4, "little")
        payload[62:66] = (2478000000).to_bytes(4, "little")
        payload[66:68] = (236).to_bytes(2, "little")
        payload[68:72] = (9080).to_bytes(4, "little")
        status = mgmt.decode_status(payload)
        self.assertEqual(mgmt.STATUS_LEN, 91)
        self.assertEqual((status.rssi_dbm, status.peer_rssi_dbm, status.temperature_centi_c),
                         (-48, -51, -125))
        self.assertEqual((status.tx_frames, status.frequency_hz, status.hop_index),
                         (1024, 2478000000, 236))
        output = mgmt.status_text(payload)
        for text in ("-48 dBm", "-51 dBm", "97%", "7 dBm", "-1.25 C",
                     "TX 1024, RX 1000", "2478.000 MHz", "9.080 ms"):
            self.assertIn(text, output)
        payload[28:30] = (-32768).to_bytes(2, "little", signed=True)
        self.assertIn("unavailable", mgmt.status_text(payload))
        self.assertEqual(mgmt.decode_status(bytes(90)).data_phy, 255)
        self.assertIn("not reported (upgrade device firmware)", mgmt.status_text(bytes(90)))
        payload[90] = 3
        self.assertIn("LoRa SF6 / BW 1 MHz", mgmt.status_text(payload))
        for length in (89, mgmt.STATUS_LEN + 1):
            with self.assertRaises(ValueError):
                mgmt.status_text(bytes(length))

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
            rc = mgmt.talk("fake", 115200, mgmt.encode(mgmt.CMD_IMAGE_HASH), True, 1, None,
                           keep=True)
        self.assertEqual(rc, 0)
        self.assertEqual(output.getvalue(), digest.hex() + "\n")

    def test_status_radio_units_validity_and_error_bits(self):
        payload = bytearray(mgmt.STATUS_LEN)
        payload[72:74] = (-125).to_bytes(2, "little", signed=True)
        payload[74:76] = (3125).to_bytes(2, "little", signed=True)
        payload[76:78] = (3301).to_bytes(2, "little")
        payload[78:84] = bytes([1, 0x18, 2, 2, 1, 0x3f])
        payload[84:86] = (0x8401).to_bytes(2, "little")
        payload[86:90] = (0xfedcba98).to_bytes(4, "little")
        output = mgmt.status_text(payload)
        for text in ("LR2021 snapshot\n", "-1.25 C", "31.25 C", "3.301 V (3301 mV)",
                     "major 0x01, minor 0x18", "standby XOSC", "power-on/brown-out",
                     "0xfedcba98", "0x8401", "HF XOSC start",
                     "measurement ADC calibration", "SRC tolerance"):
            self.assertIn(text, output)
        # Failed measurements must not display stale numeric values as current.
        payload[83] = 0
        output = mgmt.status_text(payload)
        sensors = output.split("Sensors\n")[1].split("\n\n")[0]
        radio = output.split("LR2021 snapshot\n")[1]
        self.assertEqual(sensors.count("unavailable"), 3)
        self.assertEqual(radio.count("unavailable"), 6)
        self.assertNotIn("3.301", sensors)
        # Individual validity bits work independently; valid zero is a reading.
        payload[83] = 1
        payload[72:74] = b"\0\0"
        output = mgmt.status_text(payload)
        sensors = output.split("Sensors\n")[1].split("\n\n")[0]
        self.assertIn("0.00 C", sensors)
        self.assertEqual(sensors.count("unavailable"), 2)
        with self.assertRaises(ValueError):
            mgmt.status_text(bytes(72))

    def test_status_full_size_serial_response(self):
        payload = bytes(mgmt.STATUS_LEN)
        frame = mgmt.encode(mgmt.STATUS_RESP, payload)
        self.assertEqual(len(frame), 97)
        link = Mock()
        response = io.BytesIO(frame)
        link.read.side_effect = response.read
        self.assertEqual(mgmt.read_response(link), (mgmt.STATUS_RESP, payload))

class FramingTests(unittest.TestCase):
    def test_protocol_version_matches_firmware(self):
        header = (REPO / "include/zlrs/mgmt/mgmt.h").read_text()
        version = re.search(r"#define\s+ZLRS_MGMT_VERSION\s+(\d+)U", header)
        self.assertIsNotNone(version)
        self.assertEqual(mgmt.VERSION, int(version[1]))

    def test_firmware_frame_vectors(self):
        cases = [
            ("enter", mgmt.CMD_ENTER, b"", "a55a050000f0"),
            ("status", mgmt.CMD_STATUS, b"", "a55a050100fb"),
            ("pair", mgmt.CMD_PAIR, b"", "a55a050200e6"),
            ("erase-pair", mgmt.CMD_ERASE_PAIR, b"", "a55a050300ed"),
            ("set-phrase", mgmt.CMD_SET_PHRASE, b"hello", "a55a05050568656c6c6f3b"),
            ("set-key-hex", mgmt.CMD_SET_KEY_HEX, b"00" * 32, None),
            ("set-baud", mgmt.CMD_SET_BAUD, bytes([0x00, 0x25, 0x06, 0x00]), "a55a05080400250600da"),
            ("set-region", mgmt.CMD_SET_REGION, bytes([3]), "a55a0509010340"),
            ("set-role", mgmt.CMD_SET_ROLE, bytes([1]), "a55a050a01016f"),
            ("set-profile", mgmt.CMD_SET_PROFILE, bytes([2]), "a55a050b010246"),
            ("new-dev-id", mgmt.CMD_NEW_DEV_ID, b"", "a55a050d008f"),
            ("factory-reset", mgmt.CMD_FACTORY_RESET, b"", "a55a050f0099"),
            ("recovery", mgmt.CMD_RECOVERY, b"", "a55a05100040"),
            ("build", mgmt.CMD_BUILD, b"", "a55a050600ca"),
            ("image-hash", mgmt.CMD_IMAGE_HASH, b"", "a55a050700c1"),
            ("confirm", mgmt.CMD_CONFIRM, b"", "a55a0511004b"),
            ("exit", mgmt.CMD_EXIT, b"", "a55a050e0092"),
        ]
        for name, command, payload, expected in cases:
            with self.subTest(command=name):
                frame = mgmt.encode(command, payload)
                self.assertEqual(mgmt.decode(frame), (command, payload))
                if expected is not None:
                    self.assertEqual(frame.hex(), expected)

    def test_known_frame_decode(self):
        self.assertEqual(mgmt.decode(bytes.fromhex("a55a05050568656c6c6f3b")),
                         (mgmt.CMD_SET_PHRASE, b"hello"))


    def test_single_module_is_a_target_list(self):
        args = argparse.Namespace(port=None, serial=None, all=False)
        with patch.object(mgmt, "app_ports", return_value=["/dev/serial/by-id/usb-fake-if02"]):
            self.assertEqual(mgmt.pick_targets(args, argparse.ArgumentParser()),
                             ["/dev/serial/by-id/usb-fake-if02"])
