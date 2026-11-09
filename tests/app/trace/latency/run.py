#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Control MCU-only ping/echo and export automatically triggered rolling CTF snapshots.

Install the latency fixture on both paired boards first. DATA CDCs
are never opened here. --delay-us is an explicitly labelled validation injection.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import time

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / 'tools'))
import zlrs_mgmt as mgmt
from zlrs_smp import request
from zlrs_trace import command_fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True, help='initiator USB serial')
    parser.add_argument('--peer', required=True, help='echo MCU USB serial')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--threshold-us', type=int, default=20000)
    parser.add_argument('--delay-us', type=int, default=0)
    parser.add_argument('--launch-delay-us', type=int, default=0,
                        help='test injection: postpone TX alarm without moving the radio deadline')
    parser.add_argument('--seconds', type=float, default=60)
    args = parser.parse_args()
    if (not 1000 <= args.threshold_us <= 100000 or not 0 <= args.delay_us <= 100000
            or not 0 <= args.launch_delay_us <= 1000 or args.seconds <= 0):
        parser.error('invalid threshold, injection delay or timeout')
    args.out.mkdir(parents=True, exist_ok=True)
    identities = [args.serial, args.peer]
    ports = []
    for identity in identities:
        matches = mgmt.app_ports(identity)
        if len(matches) != 1:
            parser.error(f'expected one management port for {identity}: {matches}')
        ports.append(matches[0])
    if ports[0] == ports[1]:
        parser.error('initiator and peer must be different devices')

    def bench(index, fields=None):
        result = request(ports[index], 69, fields or {'op': 0})
        if result['result'] != 0:
            raise RuntimeError(result)
        return result

    def trace(index, fields=None):
        result = request(ports[index], mgmt.GROUP,
                         command_fields(None if fields is None else fields["op"],
                                        50 if fields is None else fields.get("ms", 50)))
        if result['result'] != 0:
            raise RuntimeError(result)
        return result

    deadline = time.monotonic() + 30
    while True:
        statuses = []
        for port in ports:
            with mgmt.ManagementClient(port) as client:
                statuses.append(mgmt.read_status(client))
        if all(s.state == 4 and s.flags & 4 and not s.flags & 32 for s in statuses):
            break
        if time.monotonic() >= deadline:
            raise TimeoutError('peers are not ready for DATA')
        time.sleep(.5)
    for i in range(2):
        bench(i, {'op': 2})
        state = trace(i)['state']
        if state == 1:
            trace(i, {'op': 1})
            state = 2
        if state == 2:
            trace(i, {'op': 3})
        trace(i, {'op': 4})
    time.sleep(5)  # Exercise rolling overwrite before any probe is offered.
    deadline = time.monotonic() + args.seconds
    samples = []
    try:
        bench(1, {'op': 1, 'role': 1, 'threshold_us': args.threshold_us,
                  'delay_us': args.delay_us, 'launch_delay_us': args.launch_delay_us})
        bench(0, {'op': 1, 'role': 0, 'threshold_us': args.threshold_us,
                  'launch_delay_us': args.launch_delay_us})
        while True:
            states = [trace(i) for i in range(2)]
            sample = {'host_monotonic': time.monotonic(), 'trace': states,
                      'bench': [bench(i) for i in range(2)]}
            samples.append(sample)
            if any(s['state'] == 2 and s['error'] != 0 for s in states):
                raise RuntimeError(f'RAM frozen but persistence failed: {states}')
            if all(s['state'] == 4 for s in states):
                break
            if time.monotonic() >= deadline:
                raise TimeoutError(f'no complete two-MCU trigger: {sample}')
            time.sleep(.25)
        for i in range(2):
            assert states[i]['reason'] == 4 and states[i]['dropped'] == 0, states[i]
            assert sample['bench'][i]['triggered'] and sample['bench'][i]['bad'] == 0, sample
            assert sample['bench'][i]['overwritten'] > 0, 'rolling history was not exercised'
        metadata = REPO.parent / 'zephyr/subsys/tracing/ctf/tsdl/metadata'
        spec = importlib.util.spec_from_file_location('zephyr_viewer', REPO.parent / 'zephyr/scripts/tracing/trace_viewer.py')
        viewer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(viewer)
        exports = []
        for i, identity in enumerate(identities):
            folder = args.out / identity
            folder.mkdir(exist_ok=True)
            with mgmt.management_session(ports[i], 115200, 10) as client:
                for filename in ('capture.json', 'channel0_0'):
                    (folder / filename).write_bytes(client.download(f"/trace/{states[i]['slot']}/{filename}"))
            shutil.copyfile(metadata, folder / 'metadata')
            info = json.loads((folder / 'capture.json').read_text())
            raw = (folder / 'channel0_0').read_bytes()
            assert len(raw) == info['capacity']
            assert hashlib.sha256(raw).hexdigest() == info['sha256']
            definitions, _ = viewer.build_event_defs(str(folder / 'metadata'))
            parsed = viewer.parse_trace(str(folder / 'channel0_0'), definitions)
            named = [{'ts_ns': e.ts, **e.fields} for e in parsed.events if e.name == 'named_event']
            trigger = next(e for e in named if e['name'] == 'latency_trigger')
            assert any(e['name'] == 'cap_end' for e in named)
            (folder / 'events.json').write_text(json.dumps(named, indent=2))
            exports.append({'serial': identity, 'retained_ms': (parsed.t1 - parsed.t0) / 1e6,
                            'events': len(parsed.events), 'trigger_seq': trigger['arg0'],
                            'trigger_us': trigger['arg1'], 'artifact': info})
        result = {'threshold_us': args.threshold_us, 'injected_echo_delay_us': args.delay_us,
                  'injected_launch_delay_us': args.launch_delay_us,
                  'bench': sample['bench'], 'exports': exports,
                  'same_probe': exports[0]['trigger_seq'] == exports[1]['trigger_seq']}
        (args.out / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result, indent=2))
    finally:
        (args.out / 'polls.json').write_text(json.dumps(samples, indent=2))
        failed = sys.exc_info()[0] is not None
        errors = []
        for i in range(2):
            try:
                bench(i, {'op': 2})
            except Exception as error:
                errors.append({'serial': identities[i], 'error': str(error)})
        if errors:
            (args.out / 'cleanup-errors.json').write_text(json.dumps(errors, indent=2))
            if not failed:
                raise RuntimeError(errors)


if __name__ == '__main__':
    main()
