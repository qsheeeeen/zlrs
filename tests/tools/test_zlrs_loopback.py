# SPDX-License-Identifier: Apache-2.0
import unittest

import zlrs_loopback as loopback
import zlrs_mgmt as mgmt


class StatusSamplingTests(unittest.TestCase):
    def test_grouped_status_preserves_loopback_measurements(self):
        values = mgmt.Status(*([0] * len(mgmt.STATUS_FIELDS)))._replace(
            dev_id=b"\x01\x02\x03\x04", peer_id=b"\x05\x06\x07\x08",
            role=1, state=4, flags=7, data_phy=4, lq_percent=98,
            rssi_dbm=-105, peer_rssi_dbm=-104, peer_lq_percent=97,
            tx_frames=1024, rx_frames=1000, tx_retries=3,
            rx_crc_errors=24, rx_bad_tag=1, radio_fail=4, missed_slots=2,
            data_drop_bytes=5)
        text = mgmt.status_text(mgmt.STATUS_STRUCT.pack(*values))
        parsed = loopback.parse_status({"captured_at": "now", "status": text, "ports": ""})
        self.assertEqual(parsed, {
            "captured_at": "now", "role": "Follower", "band": "2.4 GHz",
            "modulation": "LoRa SF7 / BW 1 MHz", "state": 4, "paired": True,
            "lq_percent": 98, "rssi_dbm": -105, "peer_rssi_dbm": -104,
            "peer_lq_percent": 97, "tx_frames": 1024, "rx_frames": 1000,
            "retries": 3, "crc_errors": 24, "tag_errors": 1,
            "radio_failures": 4, "missed_slots": 2, "rf_drop_bytes": 5})

    def test_legacy_status_remains_readable(self):
        parsed = loopback.parse_status({"captured_at": "now", "ports": "", "status": """
role   : Leader
band   : 2.4 GHz
link   : state 4, lq 99%, paired 1
signal : RSSI -48 dBm
peer   : RSSI -51 dBm, lq 97%
frames : TX 1024, RX 1000, retry 3
errors : CRC 24, tag 1, radio 4, missed slots 2
data   : RF drop 5 B
"""})
        self.assertEqual(parsed["state"], 4)
        self.assertEqual(parsed["peer_rssi_dbm"], -51)
        self.assertEqual(parsed["rf_drop_bytes"], 5)
        self.assertNotIn("modulation", parsed)
