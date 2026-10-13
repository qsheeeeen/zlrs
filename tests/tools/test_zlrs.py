# SPDX-License-Identifier: Apache-2.0
import unittest
from unittest.mock import patch

from support import TOOLS, invoke
import zlrs as entry


class EntryTests(unittest.TestCase):
    def test_help_lists_all_tools_and_excludes_bringup(self):
        rc, output = invoke(entry, "--help")
        self.assertEqual(rc, 0)
        for name in ("status", "set-role", "list", "update", "recovery"):
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

    def test_24_band_command(self):
        with patch.object(entry.mgmt, "talk", return_value=0) as talk:
            rc, _ = invoke(entry, "set-band", "2.4", "--port", "fake")
        self.assertEqual(rc, 0)
        self.assertEqual(entry.mgmt.decode(talk.call_args_list[0].args[2]),
                         (entry.mgmt.CMD_SET_BAND, b"\x00"))
        self.assertEqual(invoke(entry, "set-band", "invalid")[0], 2)
        status = bytearray(entry.mgmt.STATUS_LEN)
        status[3] = 0x00
        self.assertIn("2.4 GHz", entry.mgmt.status_text(status))

    def test_subg_band_command(self):
        with patch.object(entry.mgmt, "talk", return_value=0) as talk:
            rc, _ = invoke(entry, "set-band", "subg", "--port", "fake")
        self.assertEqual(rc, 0)
        self.assertEqual(entry.mgmt.decode(talk.call_args_list[0].args[2]),
                         (entry.mgmt.CMD_SET_BAND, b"\x01"))
        status = bytearray(entry.mgmt.STATUS_LEN)
        status[3] = 0x08
        self.assertIn("sub-GHz", entry.mgmt.status_text(status))

    def test_auto_band_command_and_live_status(self):
        with patch.object(entry.mgmt, "talk", return_value=0) as talk:
            rc, _ = invoke(entry, "set-band", "auto", "--port", "fake")
        self.assertEqual(rc, 0)
        self.assertEqual(entry.mgmt.decode(talk.call_args_list[0].args[2]),
                         (entry.mgmt.CMD_SET_BAND, b"\x02"))
        status = bytearray(entry.mgmt.STATUS_LEN)
        status[3] = 0x78
        output = entry.mgmt.status_text(status)
        for value in ("sub-GHz", "auto", "searching", "scheduled"):
            self.assertIn(value, output)

    def test_subcommand_help_uses_public_name(self):
        for command in ("status", "update", "recovery"):
            with self.subTest(command=command):
                rc, output = invoke(entry, command, "--help")
                self.assertEqual(rc, 0)
                self.assertIn("zlrs.py " + command, output)

    def test_list_does_not_open_serial_port(self):
        with patch.object(entry.mgmt, "app_ports", return_value=["fake-if02"]), \
                patch.object(entry.mgmt, "talk") as talk:
            rc, output = invoke(entry, "list")
        self.assertEqual(rc, 0)
        self.assertIn("fake-if02", output)
        talk.assert_not_called()
