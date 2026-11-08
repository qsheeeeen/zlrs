# SPDX-License-Identifier: Apache-2.0
import contextlib
import io
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import zlrs
import zlrs_smp
import zlrs_trace


class TraceControlTests(unittest.TestCase):
    def invoke(self, *arguments):
        parser = zlrs.build_parser()
        args = parser.parse_args(["trace", *arguments])
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = args.handler(args=args, parser=args.command_parser)
        return result, json.loads(output.getvalue())

    def test_start_uses_smp_without_entering_management(self):
        with patch.object(zlrs_trace, "request", return_value={"result": 0, "state": 1}) as request, \
                patch.object(zlrs_trace.mgmt, "talk") as management:
            result, response = self.invoke("start", "50", "--port", "smp-port")
        self.assertEqual((result, response["state"]), (0, "capturing"))
        request.assert_called_once_with("smp-port", 64, {"op": 0, "ms": 50},
                                        timeout=10.0, baud=115200)
        management.assert_not_called()

    def test_arm_uses_fixture_rolling_operation_without_duration(self):
        with patch.object(zlrs_trace, "request", return_value={"result": 0, "state": 1}) as request:
            result, response = self.invoke("arm", "--port", "smp-port")
        self.assertEqual((result, response["state"]), (0, "capturing"))
        request.assert_called_once_with("smp-port", 64, {"op": 4}, timeout=10.0, baud=115200)

    def test_status_is_read_and_uses_management_cdc(self):
        with patch.object(zlrs_trace.mgmt, "pick_targets", return_value=["/dev/usb-device-if02"]), \
                patch.object(zlrs_trace, "request", return_value={"result": 0, "state": 2}) as request:
            self.invoke("status", "--serial", "device")
        request.assert_called_once_with("/dev/usb-device-if02", 64, None, timeout=10.0, baud=115200)

    def test_operation_failure_is_reported(self):
        with patch.object(zlrs_trace, "request", return_value={"result": -16, "state": 1}):
            with self.assertRaisesRegex(ValueError, r"failed \(-16\)"):
                self.invoke("start", "--port", "smp-port")

    def test_duration_cannot_be_sent_with_stop(self):
        with patch.object(zlrs_trace, "request") as request, \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.invoke("stop", "50", "--port", "smp-port")
        request.assert_not_called()

    def test_native_smp_round_trip_returns_only_json_payload(self):
        try:
            import cbor2
            import smpclient
        except ImportError:
            self.skipTest("smpclient serial extra is not installed")
        testcase = self

        class Client:
            def __init__(self, *args, **kwargs):
                pass

            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                pass

            async def request(self, request):
                testcase.assertEqual(request.header.group_id, 64)
                testcase.assertEqual(cbor2.loads(request.BYTES[8:]), {"op": 0, "ms": 50})
                return request._Response(result=0, state=1, id=3)

        with patch.object(smpclient, "SMPClient", Client):
            result = zlrs_smp.request("unused", 64, {"op": 0, "ms": 50})
        self.assertEqual(json.loads(json.dumps(result)), {"result": 0, "state": 1, "id": 3})


class TraceRollbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = Path(__file__).resolve().parents[1] / "app/trace/hardware/run.py"
        spec = importlib.util.spec_from_file_location("hardware_trace_runner", path)
        cls.runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.runner)

    def setUp(self):
        self.acceptance = self.runner.Acceptance.__new__(self.runner.Acceptance)
        self.acceptance.result = {}

    def test_reset_failure_does_not_skip_peer_or_verification(self):
        with patch.object(self.runner, "image_hash", return_value="test"), \
                patch.object(self.acceptance, "reboot", side_effect=[OSError("USB"), None]) as reset, \
                patch.object(self.runner, "wait_hash") as verify:
            self.acceptance.restore({"a": "old-a", "b": "old-b"})
        self.assertEqual(reset.call_count, 2)
        self.assertEqual(verify.call_count, 2)
        self.assertTrue(self.acceptance.result["restored"])

    def test_failed_verification_is_reported_after_checking_both_peers(self):
        with patch.object(self.runner, "image_hash", return_value="test"), \
                patch.object(self.acceptance, "reboot"), \
                patch.object(self.runner, "wait_hash", side_effect=[TimeoutError("USB"), None]) as verify:
            with self.assertRaisesRegex(RuntimeError, "restoration failed"):
                self.acceptance.restore({"a": "old-a", "b": "old-b"})
        self.assertEqual(verify.call_count, 2)
        self.assertFalse(self.acceptance.result["restored"])
