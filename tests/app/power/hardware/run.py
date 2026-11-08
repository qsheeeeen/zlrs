#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Two-board power acceptance with live native MCUmgr observations and real USB DATA.

Both boards must already run the same confirmed image, retain matching pairing,
and have opposite roles. --faults additionally needs the test-only power image.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import random
import sys
import time

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools'))
import zlrs_mgmt as mgmt
import zlrs_update as update
from zlrs_smp import request
from smpclient.requests.os_management import ResetWrite


def port(identity, interface='02'):
    return f'/dev/serial/by-id/usb-zlrs_ZLRS_{identity}-if{interface}'


def both(identities, function):
    with ThreadPoolExecutor(2) as pool:
        return dict(zip(identities, pool.map(function, identities)))


def observe(identity):
    with mgmt.ManagementClient(port(identity), timeout=5) as client:
        return client.request(mgmt.CMD_OBSERVE)


def reboot(identity):
    with mgmt.ManagementClient(port(identity), timeout=5) as client:
        client._request(ResetWrite(boot_mode=0))
    deadline = time.monotonic() + 10
    while Path(port(identity)).exists() and time.monotonic() < deadline:
        time.sleep(.05)
    assert not Path(port(identity)).exists(), 'USB never detached'
    assert update.wait_for_by_id('ZLRS', 60, exclude='MCUBOOT', serial=identity, exact_serial=True)


def duplex(identities, size, paced=False):
    import serial
    payloads = [random.Random(seed).randbytes(size) for seed in (317, 641)]
    started = time.monotonic()
    def receive(device):
        data = bytearray()
        deadline = time.monotonic() + 180
        while len(data) < size and time.monotonic() < deadline:
            data.extend(device.read(size - len(data)))
        return bytes(data)
    def transmit(device, data):
        if not paced:
            return device.write(data)
        count = 0
        for offset in range(0, len(data), 128):
            count += device.write(data[offset:offset + 128])
            time.sleep(.1)
        return count
    with serial.Serial(port(identities[0], '00'), 115200, timeout=.1, write_timeout=180) as first, \
         serial.Serial(port(identities[1], '00'), 115200, timeout=.1, write_timeout=180) as second:
        first.reset_input_buffer()
        second.reset_input_buffer()
        with ThreadPoolExecutor(4) as pool:
            rx_first = pool.submit(receive, first)
            rx_second = pool.submit(receive, second)
            tx_first = pool.submit(transmit, first, payloads[0])
            tx_second = pool.submit(transmit, second, payloads[1])
            assert tx_first.result() == size and tx_second.result() == size
            assert rx_first.result() == payloads[1], 'Follower -> Leader DATA differs'
            assert rx_second.result() == payloads[0], 'Leader -> Follower DATA differs'
    return {'bytes_each_direction': size, 'seconds': time.monotonic() - started,
            'sha256': [hashlib.sha256(data).hexdigest() for data in payloads]}


def connected(records):
    return all(item['status']['state'] == 4 and item['status']['flags'] & 0x67 == 7
               for item in records.values())


def at_floor(records):
    return connected(records) and all(item['status']['active_power_dbm'] ==
               item['status']['power_floor_dbm'] for item in records.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--leader', required=True)
    parser.add_argument('--follower', required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--faults', action='store_true')
    parser.add_argument('--faults-only', action='store_true', help='run fault cases after a fresh reboot')
    parser.add_argument('--weak-only', action='store_true', help='run only weak-RSSI downgrade/recovery')
    args = parser.parse_args()
    args.faults_only = args.faults_only or args.weak_only
    args.faults = args.faults or args.faults_only
    identities = (args.leader, args.follower)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    result = {}
    origin = time.monotonic()
    history = {}
    encode = lambda value: value.hex() if isinstance(value, bytes) else str(value)
    samples = (args.out_dir / 'samples.jsonl').open('w')
    def poll(phase):
        readings = both(identities, observe)
        samples.write(json.dumps({'seconds': time.monotonic() - origin,
                     'phase': phase, 'devices': readings}, default=encode) + '\n')
        samples.flush()
        history.setdefault(phase, []).append(readings)
        return readings
    def until(phase, condition, timeout):
        start = time.monotonic()
        while True:
            try:
                readings = poll(phase)
            except (TimeoutError, OSError):
                # USBIP can expose CDC nodes before the restarted SMP endpoint is ready.
                if phase not in ('startup', 'fault_restart') or time.monotonic() - start >= timeout:
                    raise
                time.sleep(.2)
                continue
            if condition(readings):
                return {'seconds': time.monotonic() - start, 'devices': readings}
            assert time.monotonic() - start < timeout, (phase, readings)
            time.sleep(.25)
    def stream(phase, size, paced=False):
        with ThreadPoolExecutor(1) as pool:
            job = pool.submit(duplex, identities, size, paced)
            while not job.done():
                poll(phase)
                time.sleep(.25)
            return job.result()
    try:
        for identity in identities:
            reboot(identity)
        result['connected'] = until('startup', connected, 40)
        result['floor'] = until('lowering', at_floor, 45)
        print('Both devices reached their TX floor', result['floor']['seconds'], flush=True)
        if not args.faults_only:
            baseline = poll('idle')
            until('idle', lambda r: all(r[key]['uptime_ms'] - baseline[key]['uptime_ms'] >= 70000
                  for key in identities), 80)
            idle = poll('idle')
            assert at_floor(idle), idle
            assert all(idle[key]['status']['peer_lq_generation'] >= 2 for key in identities), idle
            result['idle'] = idle
            print('Idle quality windows observed; both remain at the TX floor', flush=True)
            result['duplex'] = stream('duplex', 2 * 1024 * 1024)
            result['after_duplex'] = poll('after_duplex')
            assert connected(result['after_duplex'])
            assert all(result['after_duplex'][key]['status']['data_drop_bytes'] == 0 for key in identities)
            print('Duplex byte-exact DATA passed', result['duplex'], flush=True)
            result['post_data_floor'] = until('post_data', at_floor, 45)
            follower_before = result['post_data_floor']['devices'][identities[1]]['status']
            with mgmt.ManagementClient(port(identities[0]), timeout=5) as client:
                client.request(mgmt.CMD_ENTER)
                try:
                    result['feedback_loss'] = until('feedback_loss', lambda r:
                        r[identities[1]]['status']['active_power_dbm'] ==
                        r[identities[1]]['status']['tx_power_dbm'] and
                        r[identities[1]]['status']['power_fallbacks'] > follower_before['power_fallbacks'], 5)
                finally:
                    client.request(mgmt.CMD_EXIT)
            print('Feedback loss restored Follower maximum', flush=True)
            result['recovery'] = until('recovery', at_floor, 90)
        if args.faults:
            def fault(identity, every=0, rssi=0, duration=0):
                return request(port(identity), 67, {'drop_every': every,
                    'rssi_dbm': rssi, 'duration_ms': duration})
            if not args.weak_only:
                before = poll('occasional_loss')
                fault(identities[1], every=64, duration=60000)
                result['occasional_loss_data'] = stream('occasional_loss', 32768, paced=True)
                fault(identities[1])
                after = poll('occasional_loss')
                assert connected(after), after
                observed = [row[identities[0]]['status'] for row in history['occasional_loss']]
                bad_windows = {row['peer_lq_generation'] for row in observed if row['peer_lq_percent'] < 95}
                raises = after[identities[0]]['status']['power_raises'] - before[identities[0]]['status']['power_raises']
                assert raises <= len(bad_windows), (raises, bad_windows)
                assert any(95 <= row['peer_lq_percent'] <= 100 for row in observed), observed[-1]
                assert all(row['data_phy'] == 0 for row in observed), observed[-1]
                result['occasional_loss'] = {'devices': after, 'new_bad_windows': len(bad_windows),
                                            'raises': raises}
                result['occasional_loss_floor'] = until('occasional_loss_recovery', at_floor, 20)
                print('Occasional loss: raises bounded by new poor windows; TX returns to floor', flush=True)
                fault(identities[1], every=8, duration=30000)
                result['low_quality_data'] = stream('low_quality', 65536)
                fault(identities[1])
                low = poll('cached_low_quality')
                assert low[identities[0]]['status']['peer_lq_percent'] < 95, low
                baseline_raises = low[identities[0]]['status']['power_raises']
                started = time.monotonic()
                while time.monotonic() - started < 12:
                    readings = poll('cached_low_quality')
                    assert readings[identities[0]]['status']['power_raises'] == baseline_raises, readings
                    time.sleep(.25)
                result['cached_low_quality'] = readings
                assert at_floor(readings), readings
                print('Cached low LQ did not repeat a raise; TX returned to floor', flush=True)
                fault(identities[1], every=-3, duration=30000)
                result['severe_loss_data'] = stream('severe_loss', 65536)
                fault(identities[1])
                result['severe_loss'] = poll('severe_loss')
                assert any(row[identities[0]]['status']['peer_lq_percent'] < 50
                           for row in history['severe_loss']), 'severe LQ not observed'
                assert any(row[identities[0]]['status']['active_power_dbm'] ==
                           row[identities[0]]['status']['tx_power_dbm']
                           for row in history['severe_loss']), 'no severe-loss ceiling recovery'
                # Restart at the fastest PHY before the independent RSSI fault.
                for identity in identities:
                    reboot(identity)
                until('fault_restart', connected, 40)
                until('fault_restart', at_floor, 45)
            both(identities, lambda identity: fault(identity, rssi=-92, duration=8000))
            result['weak_rssi'] = until('weak_rssi', lambda r: connected(r) and all(
                item['status']['active_power_dbm'] == item['status']['tx_power_dbm']
                for item in r.values()), 5)
            result['weak_rssi_downgrade'] = until('weak_rssi', lambda r: connected(r) and all(
                item['status']['data_phy'] == 1 for item in r.values()), 15)
            both(identities, fault)
            result['rssi_recovery_data'] = stream('rssi_recovery', 512 * 1024)
            result['rssi_recovery'] = until('rssi_recovery', lambda r: at_floor(r) and all(
                item['status']['data_phy'] == 0 for item in r.values()), 120)
            print('Weak RSSI restored maximum and downgraded; healthy link recovered fastest PHY', flush=True)
            result['wire'] = both(identities, lambda identity: request(port(identity), 67))
            assert all(item['toggles'] >= 2 and item['identical_toggles'] >= 1
                       for item in result['wire'].values()), result['wire']
        result['passed'] = True
    finally:
        if args.faults:
            try:
                both(identities, lambda identity: request(port(identity), 67,
                    {'drop_every': 0, 'rssi_dbm': 0, 'duration_ms': 0}))
            except Exception as error:
                result['cleanup_error'] = str(error)
                result['passed'] = False
        samples.close()
        (args.out_dir / 'result.json').write_text(json.dumps(result, indent=2, default=encode) + '\n')
    if result.get('passed') is not True:
        raise RuntimeError('Power acceptance or fault cleanup did not pass')


if __name__ == '__main__':
    main()
