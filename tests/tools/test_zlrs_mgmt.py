# SPDX-License-Identifier: Apache-2.0
import contextlib
import errno
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch
from support import invoke
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
        request = m.product_request_type({"op": m.CMD_SET_ROLE, "value": b"\x01"})
        self.assertEqual(request._GROUP_ID, 66)
        self.assertEqual(request._COMMAND_ID, 0)
        self.assertEqual(request.model_extra, {"op": m.CMD_SET_ROLE, "value": b"\x01"})

    def test_native_error_response_retains_product_group(self):
        from smp import header, error
        request = m.product_request_type({"op": 126})
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
