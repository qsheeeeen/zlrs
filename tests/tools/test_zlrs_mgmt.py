# SPDX-License-Identifier: Apache-2.0
import contextlib
import errno
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch
from support import invoke
import zlrs as entry
import zlrs_mgmt as m


class ManagementTests(unittest.TestCase):
    def test_config_uses_product_operation_and_preserves_uart_baud(self):
        with patch.object(m, "talk", return_value=0) as talk:
            rc, _ = invoke(m, "--port", "fake", "--baud", "115200", "set-baud", "230400")
        self.assertEqual(rc, 0)
        self.assertEqual(talk.call_args.args[1], 115200)
        self.assertEqual(talk.call_args.args[2], (m.CMD_SET_BAUD, (230400).to_bytes(4, "little")))

    def test_invalid_phrase_and_key_rejected_before_open(self):
        for args in [("set-phrase", ""), ("set-phrase", "x" * 33),
                     ("set-key-hex", "gg" * 32), ("set-key-hex", "ab")]:
            with self.subTest(args=args), patch.object(m, "ManagementClient") as client:
                self.assertEqual(invoke(m, "--port", "fake", *args)[0], 2)
                client.assert_not_called()

    def test_observation_does_not_enter_or_exit_maintenance(self):
        with patch.object(m, "ManagementClient") as factory:
            client = factory.return_value.__enter__.return_value
            client.entered = False
            with m.management_session("fake", 115200, 1, observe=True):
                pass
        client.request.assert_not_called()

    def test_session_exits_after_operation_failure(self):
        with patch.object(m, "ManagementClient") as factory:
            client = factory.return_value.__enter__.return_value
            client.entered = True
            with self.assertRaisesRegex(ValueError, "failure"):
                with m.management_session("fake", 115200, 1):
                    raise ValueError("failure")
        self.assertEqual([call.args[0] for call in client.request.call_args_list], [m.CMD_ENTER, m.CMD_EXIT])

    def test_no_enter_still_exits_existing_session(self):
        with patch.object(m, "ManagementClient") as factory:
            client = factory.return_value.__enter__.return_value
            client.entered = False
            with m.management_session("fake", 115200, 1, enter=False):
                pass
        client.request.assert_called_once_with(m.CMD_EXIT)

    def test_query_session_policy_matches_dry_run(self):
        status = dict.fromkeys(m.STATUS_FIELDS, 0)
        status.update(dev_id=b"1234", peer_id=b"5678")
        for options, expected in [
            ((), [m.CMD_STATUS]),
            (("--verbose",), [m.CMD_STATUS]),
            (("--no-enter",), [m.CMD_STATUS, m.CMD_EXIT]),
            (("--refresh",), [m.CMD_ENTER, m.CMD_STATUS, m.CMD_EXIT]),
            (("--keep",), [m.CMD_ENTER, m.CMD_STATUS]),
            (("--no-enter", "--keep"), [m.CMD_STATUS]),
        ]:
            with self.subTest(options=options), patch.object(m, "ManagementClient") as factory:
                client = factory.return_value.__enter__.return_value
                client.entered = False

                def respond(command, payload=b""):
                    if command == m.CMD_ENTER:
                        client.entered = True
                    elif command == m.CMD_EXIT:
                        client.entered = False
                    return {"status": status, "uptime_ms": 100, "hardware_ms": 10}

                client.request.side_effect = respond
                self.assertEqual(invoke(entry, "status", "--port", "fake", *options)[0], 0)
                self.assertEqual([call.args[0] for call in client.request.call_args_list], expected)
                rc, output = invoke(entry, "status", "--dry-run", *options)
                self.assertEqual(rc, 0)
                operations = [line.split(": ", 1)[1] for line in output.splitlines()]
                self.assertEqual(operations, [repr(m.operation(command)) for command in expected])

    def test_status_summary_and_verbose_chip_details(self):
        status = dict.fromkeys(m.STATUS_FIELDS, 0)
        status.update(dev_id=b"1234", peer_id=b"5678", radio_valid=63,
                      temperature_centi_c=2675, radio_errors=4, radio_vbat_mv=3300)
        system = {"uptime_ms": 90061000, "hardware_ms": 90060000,
                  "cpu_load_permille": 217, "cpu_window_ms": 5000}
        client = MagicMock()
        client.request.return_value = {"status": status, **system}
        with patch.object(m, "management_session", return_value=contextlib.nullcontext(client)):
            rc, output = invoke(entry, "status", "--port", "fake", "--verbose")
        self.assertEqual(rc, 0)
        for value in ("CPU 21.7% (avg 5.00s)", "up 1d 01:01:01", "26.75 C", "3.300 V",
                      "31323334", "35363738", "PLL lock"):
            self.assertIn(value, output)
        status["regions"] = (1 << 0) | (1 << 3)
        summary = m.status_text(status, system=system)
        self.assertIn("FCC915, CN433", summary)
        self.assertNotIn("EU868", summary)
        self.assertEqual(len(summary.splitlines()), 15)
        self.assertTrue(all(len(line) <= 80 for line in summary.splitlines()))
        self.assertNotIn("Device ID", summary)
        self.assertNotIn("IRQ status", summary)

    def test_status_unavailable_and_cpu_boundaries(self):
        status = dict.fromkeys(m.STATUS_FIELDS, 0)
        status.update(dev_id=b"1234", peer_id=b"5678", temperature_centi_c=-32768)
        for cpu, window, expected in ((0, 1000, "0.0%"), (1000, 1000, "100.0%"),
                                      (-34, 1000, "unavailable"), (0, 0, "unavailable")):
            with self.subTest(cpu=cpu, window=window):
                output = m.status_text(status, system={"cpu_load_permille": cpu, "cpu_window_ms": window})
                self.assertIn("CPU " + expected, output)
        output = m.status_text(status, verbose=True)
        for value in ("CPU unavailable", "MCU unavailable", "RF unavailable",
                      "XOSC unavailable", "supply unavailable", "age unavailable"):
            self.assertIn(value, output)
        self.assertNotIn("-327.68", output)
        output = m.status_text(status, system={"uptime_ms": 0x1000003e8, "hardware_ms": 0xffffff00})
        self.assertIn("age 0d 00:00:01", output)

    def test_report_is_one_smp_response(self):
        client = MagicMock()
        client.request.return_value = {"report": "complete report\n"}
        self.assertEqual(m.read_debug_report(client, 3), b"complete report\n")
        client.request.assert_called_once_with(m.CMD_DEBUG_REPORT, b"\x03")

    def test_logs_use_standard_fs_download(self):
        client = MagicMock()
        client.request.return_value = {"files": [{"path": "/lfs/log.0007", "size": 5}]}
        client.download.return_value = b"hello"
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(m, "management_session", return_value=contextlib.nullcontext(client)):
            self.assertEqual(m.dump_logs("fake", 115200, 1, directory), 0)
            self.assertEqual((Path(directory) / "log.0007.bin").read_bytes(), b"hello")
        client.download.assert_called_once_with("/lfs/log.0007")

    def test_dry_run_has_no_serial_or_file_side_effects(self):
        with patch.object(m, "ManagementClient") as client:
            self.assertEqual(invoke(m, "--dry-run", "--port", "fake", "status")[0], 0)
            self.assertEqual(invoke(m, "--dry-run", "--port", "fake", "logs")[0], 0)
        client.assert_not_called()


@unittest.skipUnless(importlib.util.find_spec("smpclient"), "smpclient not installed")
class NativeClientTests(unittest.TestCase):
    def test_product_uses_native_group_66(self):
        request = m.request_type(66, {"op": m.CMD_SET_ROLE, "value": b"\x01"})
        self.assertEqual(request._GROUP_ID, 66)
        self.assertEqual(request._COMMAND_ID, 0)
        self.assertEqual(request.model_extra, {"op": m.CMD_SET_ROLE, "value": b"\x01"})

    def test_native_error_response_retains_product_group(self):
        from smp import header, error
        request = m.request_type(66, {"op": 126})
        response_header = header.Header(op=header.OP.WRITE_RSP, version=header.Version.V2,
            flags=header.Flag.UNUSED, length=5, group_id=66, sequence=0, command_id=0)
        response = request._ErrorV1(header=response_header, rc=error.MGMT_ERR.ENOTSUP)
        decoded = request._ErrorV1.loads(response.BYTES)
        self.assertEqual(decoded.rc, error.MGMT_ERR.ENOTSUP)
        self.assertEqual(decoded.header.group_id, 66)

    def test_standard_requests_are_not_product_operations(self):
        from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite
        from smpclient.requests.os_management import ResetWrite, OSApplicationInfoRead
        client = m.ManagementClient("fake")
        reply = MagicMock()
        reply.output = "Zephyr"
        reply.images = [MagicMock(active=True, hash=b"x" * 32)]
        with patch.object(client, "_request", return_value=reply) as request:
            self.assertEqual(client.request(m.CMD_BUILD), "Zephyr")
            self.assertIsInstance(request.call_args.args[0], OSApplicationInfoRead)
            self.assertEqual(client.request(m.CMD_IMAGE_HASH), b"x" * 32)
            self.assertIsInstance(request.call_args.args[0], ImageStatesRead)
            client.request(m.CMD_CONFIRM)
            self.assertIsInstance(request.call_args.args[0], ImageStatesWrite)
            self.assertTrue(request.call_args.args[0].confirm)
            client.request(m.CMD_RECOVERY)
            self.assertIsInstance(request.call_args.args[0], ResetWrite)
            self.assertEqual(request.call_args.args[0].boot_mode, 1)
