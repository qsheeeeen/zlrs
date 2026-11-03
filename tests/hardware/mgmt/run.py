#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise native SMP management in an unconfirmed fixture; restore NVS and firmware."""
import argparse
import errno
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
import zlrs_mgmt as m
import zlrs_update as u


def exercise(port, export_dir=None, log_limit=None):
    checks = []
    with m.ManagementClient(port, timeout=90) as client:
        def command(op, value=b'', error=None):
            try:
                result = client.request(op, value)
            except m.ManagementError as exc:
                if error != exc.status:
                    raise
            else:
                if error is not None:
                    raise AssertionError(f'operation {op}: expected {error}')
                checks.append(op)
                print(f"SMP operation {op}: passed", flush=True)
                return result
            checks.append(op)
            print(f"SMP operation {op}: expected error {error}", flush=True)

        command(m.CMD_PAIR, error=-errno.EACCES)
        before = command(m.CMD_OBSERVE)
        assert before['status']['frame_period_us'] > 0
        command(m.CMD_ENTER)
        command(m.CMD_ERASE_PAIR)
        before = m.read_status(client)
        command(m.CMD_EXIT)
        time.sleep(0.2)
        command(m.CMD_ENTER)
        after = m.read_status(client)
        assert after.flags & 6 == 0
        assert after.flags & 1 == before.flags & 1 and after.dev_id == before.dev_id
        assert command(m.CMD_BUILD)
        assert len(command(m.CMD_IMAGE_HASH)) == 32
        try:
            client.request(m.CMD_CONFIRM)
        except ValueError:
            pass  # Native boot_set_next is deliberately fault-injected.
        else:
            raise AssertionError('expected native IMG confirmation failure')
        command(m.CMD_CONFIRM)  # Fixture never writes image_ok.
        command(m.CMD_SET_BAUD, (230400).to_bytes(4, 'little'))
        command(m.CMD_SET_BAUD, b'\x01', -errno.EINVAL)
        command(m.CMD_SET_REGION, b'\x03')
        command(m.CMD_SET_REGION, b'\xff', -errno.EINVAL)
        command(m.CMD_SET_ROLE, bytes([1 - after.role]), -errno.EIO)
        command(m.CMD_SET_ROLE, b'\x01')
        command(m.CMD_SET_ROLE, b'\xff', -errno.EINVAL)
        command(m.CMD_SET_PROFILE, b'\x01')
        command(m.CMD_SET_PROFILE, b'\x02', -errno.EINVAL)
        command(m.CMD_SET_BAND, b'\x01')
        command(m.CMD_SET_BAND, b'\xff', -errno.EINVAL)
        exported = {}
        files = command(m.CMD_LOG_INFO)['files']
        for item in files[-log_limit:] if log_limit else files:
            data = client.download(item['path'])
            assert len(data) == item['size']
            exported[item['path']] = len(data)
            if export_dir is not None:
                target = Path(export_dir) / Path(item['path']).name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
        try:
            client.download('/lfs/../storage')
        except Exception as exc:
            assert 'denied' in str(exc).lower() or 'err' in str(exc).lower(), str(exc)
        else:
            raise AssertionError('FS escaped read-only export paths')
        assert m.read_debug_report(client, 3)
        command(m.CMD_SET_KEY_HEX, b'a' * 63, -errno.EINVAL)
        command(m.CMD_SET_KEY_HEX, b'a' * 64)
        command(m.CMD_SET_PHRASE, b'mgmt-hardware')
        command(m.CMD_PAIR)
        command(m.CMD_ERASE_PAIR)
        assert m.read_status(client).flags & 7 == 1
        command(m.CMD_NEW_DEV_ID, b'\x01', -errno.EINVAL)
        for _ in range(10):
            command(m.CMD_NEW_DEV_ID)
        command(m.CMD_FACTORY_RESET)
        reset = m.read_status(client)
        assert reset.flags & 7 == 0 and reset.dev_id == bytes(4) and reset.peer_id == bytes(4)
        try:
            client.request(0x7e)
        except ValueError as exc:
            assert 'ENOTSUP' in str(exc), str(exc)
        else:
            raise AssertionError('unsupported operation accepted')
        command(m.CMD_EXIT)
    return {'checks': len(checks), 'exported': exported}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--image', required=True)
    args = parser.parse_args()
    port = f'/dev/serial/by-id/usb-zlrs_ZLRS_{args.serial}-if02'
    with m.ManagementClient(port) as client:
        original_hash = client.request(m.CMD_IMAGE_HASH).hex()
        original = m.read_status(client)
    subprocess.run([sys.executable, str(ROOT / 'tools/zlrs.py'), 'update', '--serial', args.serial,
                    '--image', args.image, '--no-wait', '--timeout', '45'], check=True)
    u.wait_for_by_id('ZLRS', 45, exclude='MCUBOOT', serial=args.serial, exact_serial=True)
    try:
        rc, output = u.run_mgmt_subcmd_retry(port, 'image-hash')
        assert rc == 0 and u.parse_hex_hash(output) == u.image_hash_of(Path(args.image))
        print(json.dumps(exercise(port)), flush=True)
    finally:
        with m.ManagementClient(port) as client:
            client.request(m.CMD_RECOVERY)  # Fixture restores the complete NVS snapshot.
        recovery = u.wait_for_by_id('MCUBOOT', 30, serial=args.serial, exact_serial=True)
        subprocess.run([u.find_client(), '--port', recovery, 'os', 'reset'], check=True)
        u.wait_for_by_id('ZLRS', 45, exclude='MCUBOOT', serial=args.serial, exact_serial=True)
        rc, output = u.run_mgmt_subcmd_retry(port, 'image-hash')
        assert rc == 0 and u.parse_hex_hash(output) == original_hash
        with m.ManagementClient(port) as client:
            restored = m.read_status(client)
        for field in ('dev_id', 'peer_id', 'region', 'data_baud', 'profile'):
            assert getattr(original, field) == getattr(restored, field), field
        print('Original NVS and firmware restored.', flush=True)


if __name__ == '__main__':
    main()
