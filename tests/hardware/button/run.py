#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Two-board boot-window test using native Input events and USB DATA endpoints.

Install the button fixture on both boards and configure matching keys and opposite
roles and confirm the fixture first. This exercises native longpress processing, not GPIO electrical input.
"""
import argparse
import concurrent.futures as futures
import hashlib
import json
from pathlib import Path
import random
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
import zlrs_mgmt as mgmt
import zlrs_update as update
from zlrs_smp import request
from smpclient.requests.os_management import ResetWrite


def ports(identity):
    return f'/dev/serial/by-id/usb-zlrs_ZLRS_{identity}-if02'


def both(identities, function):
    with futures.ThreadPoolExecutor(2) as pool:
        return dict(zip(identities, pool.map(function, identities)))


def status(identity):
    return request(ports(identity), 67, timeout=5)


def online(identity):
    with mgmt.ManagementClient(ports(identity), timeout=5) as client:
        return client.request(mgmt.CMD_STATUS)['status']


def reboot(identity):
    path = ports(identity)
    with mgmt.ManagementClient(path, timeout=5) as client:
        client._request(ResetWrite(boot_mode=0))
    deadline = time.monotonic() + 10
    while Path(path).exists() and time.monotonic() < deadline:
        time.sleep(.05)
    assert not Path(path).exists(), 'USB never detached after reset'
    assert update.wait_for_by_id('ZLRS', 60, exclude='MCUBOOT', serial=identity, exact_serial=True)


def data_round(identities, size=131072):
    import serial
    payloads = [random.Random(seed).randbytes(size) for seed in (317, 641)]
    started = time.monotonic()
    def receive(port):
        data = bytearray()
        deadline = time.monotonic() + 30
        while len(data) < size and time.monotonic() < deadline:
            data.extend(port.read(size - len(data)))
        return bytes(data)
    with serial.Serial(ports(identities[0]).replace('-if02', '-if00'), 115200,
                       timeout=.1, write_timeout=20) as first, \
         serial.Serial(ports(identities[1]).replace('-if02', '-if00'), 115200,
                       timeout=.1, write_timeout=20) as second:
        first.reset_input_buffer()
        second.reset_input_buffer()
        with futures.ThreadPoolExecutor(4) as pool:
            rx_first = pool.submit(receive, first)
            rx_second = pool.submit(receive, second)
            tx_first = pool.submit(first.write, payloads[0])
            tx_second = pool.submit(second.write, payloads[1])
            assert tx_first.result() == size and tx_second.result() == size
            assert rx_first.result() == payloads[1], 'Follower -> Leader DATA differs'
            assert rx_second.result() == payloads[0], 'Leader -> Follower DATA differs'
    return {'bytes_each_direction': size, 'seconds': time.monotonic() - started,
            'sha256': [hashlib.sha256(data).hexdigest() for data in payloads]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--leader', required=True)
    parser.add_argument('--follower', required=True)
    parser.add_argument('--out-dir', type=Path, required=True)
    args = parser.parse_args()
    identities = (args.leader, args.follower)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    result = {}
    try:
        both(identities, reboot)
        boot = both(identities, status)
        result['boot'] = boot
        early_at = max(item['uptime_ms'] for item in boot.values()) + 1000
        assert early_at + 1800 < 29000, boot
        both(identities, lambda identity: request(ports(identity), 67,
             {'at_ms': early_at, 'hold_ms': 1200}, timeout=5))
        deadline = time.monotonic() + 20
        while True:
            early = both(identities, status)
            if all(item['holds'] == 1 and item['state'] == 4 for item in early.values()):
                break
            assert time.monotonic() < deadline, early
            time.sleep(.15)
        result['early_pairing'] = early
        print('Early native Input longpress: paired and connected', flush=True)
        both(identities, lambda identity: request(ports(identity), 67,
             {'at_ms': 29500, 'hold_ms': 1500}, timeout=5))
        while True:
            boundary = both(identities, status)
            if min(item['uptime_ms'] for item in boundary.values()) >= 31500:
                break
            time.sleep(.2)
        assert all(not item['allowed'] and item['holds'] == 1 and item['state'] == 4
                   for item in boundary.values()), boundary
        result['crossing_deadline'] = boundary
        print('Hold crossing 30 seconds ignored; link remains connected', flush=True)
        both(identities, lambda identity: request(ports(identity), 67,
             {'at_ms': status(identity)['uptime_ms'] + 300, 'hold_ms': 1800}, timeout=5))
        result['data'] = data_round(identities)
        time.sleep(2)
        late = both(identities, status)
        assert all(not item['allowed'] and item['holds'] == 1 and item['state'] == 4
                   for item in late.values()), late
        result['after_deadline'] = late
        result['link'] = both(identities, online)
        assert all(item['flags'] & 7 == 7 for item in result['link'].values())
        result['passed'] = True
        print('Late hold ignored; bidirectional DATA verified', flush=True)
    finally:
        (args.out_dir / 'button-result.json').write_text(json.dumps(result, indent=2,
             default=lambda value: value.hex() if isinstance(value, bytes) else str(value)) + '\n')


if __name__ == '__main__':
    main()
