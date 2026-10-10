# SPDX-License-Identifier: Apache-2.0
import re
import unittest

from support import REPO
import zlrs_recovery as recovery


class RecoveryTests(unittest.TestCase):
    def test_frame_matches_firmware(self):
        request = recovery.build_request()
        self.assertEqual(request, bytes.fromhex("a55a041100c8"))
        self.assertEqual(request[3], recovery.CMD_RECOVERY)
        header = (REPO / "include/zlrs/mgmt/mgmt.h").read_text()
        version = re.search(r"#define\s+ZLRS_MGMT_VERSION\s+(\d+)U", header)
        self.assertIsNotNone(version)
        self.assertEqual(int(version[1]), recovery.VERSION)
