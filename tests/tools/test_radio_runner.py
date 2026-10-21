#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Firmware rollback must survive failures in the test or its console hooks."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("hardware_radio_runner", ROOT / "tests/hardware/radio/run.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class RollbackTests(unittest.TestCase):
    def test_request_errors_do_not_skip_peer_or_image_verification(self):
        for failure in (ValueError("invalid capture output"), OSError("USB disappeared")):
            with self.subTest(failure=failure):
                events = []
                consoles = [MagicMock(), MagicMock()]

                def request(port, text):
                    index = consoles.index(port)
                    events.append(("request", index))
                    self.assertEqual(text, "test_radio restore")
                    if index == 0:
                        raise failure

                def wait(identity, image, timeout):
                    events.append(("wait", identity))
                    self.assertEqual(image, identity + "-original")
                    self.assertEqual(timeout, 125)

                with patch.object(runner.serial, "Serial") as serial, \
                        patch.object(runner, "command", side_effect=request), \
                        patch.object(runner, "wait_image", side_effect=wait):
                    serial.side_effect = [MagicMock(__enter__=MagicMock(return_value=p)) for p in consoles]
                    result = runner.restore_images(("a", "b"), {"a": "a-original", "b": "b-original"})
                self.assertEqual(events, [("request", 0), ("request", 1), ("wait", "a"), ("wait", "b")])
                self.assertEqual(result["restored"], {"a": True, "b": True})
                self.assertEqual(result["restore_errors"], {"a": {"request": str(failure)}})

    def test_verification_failure_preserves_peer_result(self):
        with patch.object(runner.serial, "Serial"), patch.object(runner, "command") as command, \
                patch.object(runner, "wait_image", side_effect=[ValueError("unexpected reply"), None]):
            result = runner.restore_images(("a", "b"), {"a": "old-a", "b": "old-b"})
        self.assertEqual(command.call_count, 2)
        self.assertEqual(result["restored"], {"a": False, "b": True})
        self.assertEqual(result["restore_errors"], {"a": {"wait": "unexpected reply"}})


if __name__ == "__main__":
    unittest.main()
