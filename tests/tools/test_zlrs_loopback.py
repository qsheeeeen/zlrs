# SPDX-License-Identifier: Apache-2.0
import unittest
from unittest.mock import Mock, patch

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
        client = Mock()
        client.request.return_value = {"status": values._asdict()}
        with patch.object(mgmt, "status_text", return_value="any display layout"):
            sample = loopback.sample_status(client)
        client.request.assert_called_once_with(mgmt.CMD_STATUS)
        self.assertEqual(sample["status"], "any display layout")
        parsed = sample["metrics"]
        self.assertEqual(parsed, {
            "role": "Follower", "band": "2.4 GHz",
            "modulation": "LoRa SF7 / BW 1 MHz", "state": 4, "paired": True,
            "lq_percent": 98, "rssi_dbm": -105, "peer_rssi_dbm": -104,
            "peer_lq_percent": 97, "tx_frames": 1024, "rx_frames": 1000,
            "retries": 3, "crc_errors": 24, "tag_errors": 1,
            "radio_failures": 4, "missed_slots": 2, "rf_drop_bytes": 5})

    def test_unavailable_signal_is_not_a_measurement(self):
        values = mgmt.Status(*([0] * len(mgmt.STATUS_FIELDS)))._replace(lq_percent=127)
        client = Mock()
        client.request.return_value = {"status": values}
        metrics = loopback.sample_status(client)["metrics"]
        for key in ("rssi_dbm", "peer_rssi_dbm", "lq_percent", "peer_lq_percent"):
            self.assertNotIn(key, metrics)
