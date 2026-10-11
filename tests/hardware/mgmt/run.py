#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run real management commands in an unconfirmed fixture and restore NVS/firmware."""
import argparse
from pathlib import Path
import subprocess
import sys
import time
import serial


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
import zlrs_mgmt as m
import zlrs_update as u


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--image', default=str(ROOT / 'build/mgmt-hardware/mgmt/zephyr/zephyr.signed.bin'))
    args = p.parse_args()
    cli = [sys.executable, str(ROOT / 'tools/zlrs.py')]

    def run(*a):
        result = subprocess.run(cli + list(a) + ['--serial', args.serial],
                                capture_output=True, text=True, timeout=180)
        if result.returncode:
            print(result.stdout + result.stderr, flush=True)
        result.check_returncode()
        return result.stdout
    previous = u.parse_hex_hash(run('image-hash'))
    assert previous
    port = f'/dev/serial/by-id/usb-zlrs_ZLRS_{args.serial}-if02'
    original = run('status')
    print('Original hash:', previous, flush=True)
    try:
        print(run('update', '--image', args.image, '--no-wait', '--timeout', '45'), flush=True)
        port = u.wait_for_by_id('ZLRS', 45, exclude='MCUBOOT', serial=args.serial, exact_serial=True)
        assert port
        rc, output = u.run_mgmt_subcmd_retry(port, 'image-hash')
        assert rc == 0 and u.parse_hex_hash(output) == u.image_hash_of(Path(args.image)), output
        with serial.Serial(port, 115200, timeout=12) as s:

            def cmd(c, b=b'', expected=0, response=None):
                s.write(m.encode(c, b))
                got, body = m.read_response(s)
                if response is None:
                    assert got == m.ACK and body == bytes((c, expected)), (c, got, body.hex(), expected)
                else:
                    assert got == response, (c, got, body.hex())
                print(f'command {c:02x}: PASS', flush=True)
                return body
            cmd(m.CMD_ENTER)
            cmd(m.CMD_STATUS, response=m.STATUS_RESP)
            cmd(m.CMD_BUILD, response=m.BUILD_RESP)
            cmd(m.CMD_IMAGE_HASH, response=m.IMAGE_HASH_RESP)
            cmd(m.CMD_CONFIRM, expected=3)
            cmd(m.CMD_CONFIRM)
            cmd(m.CMD_SET_BAUD, (230400).to_bytes(4, 'little'))
            cmd(m.CMD_SET_BAUD, b'\x01', expected=1)
            cmd(m.CMD_SET_REGION, b'\x03')
            cmd(m.CMD_SET_REGION, b'\xff', expected=1)
            cmd(m.CMD_SET_POWER, b'\xf7', expected=3)
            cmd(m.CMD_SET_POWER, b'\xf8')
            cmd(m.CMD_SET_ROLE, b'\x01')
            cmd(m.CMD_SET_ROLE, b'\xff', expected=1)
            cmd(m.CMD_SET_PROFILE, b'\x01')
            cmd(m.CMD_SET_PROFILE, b'\x02', expected=1)
            cmd(m.CMD_SET_BAND, b'\x01')
            cmd(m.CMD_SET_BAND, b'\xff', expected=1)
            info = cmd(m.CMD_LOG_INFO, response=m.LOG_INFO_RESP)
            if info[0]:
                cmd(m.CMD_LOG_READ, b'\x00\x00\x00\x00\x00 ', response=m.LOG_DATA_RESP)
            cmd(m.CMD_LOG_READ, b'\x00', expected=1)
            cmd(m.CMD_DEBUG_REPORT, b'\x03\x00\x00', response=m.DEBUG_REPORT_RESP)
            cmd(m.CMD_SET_KEY_HEX, b'a' * 63, expected=1)
            cmd(m.CMD_SET_KEY_HEX, b'a' * 64)
            cmd(m.CMD_SET_PHRASE, b'mgmt-hardware')
            cmd(m.CMD_PAIR)
            cmd(m.CMD_ERASE_PAIR)
            cmd(m.CMD_NEW_DEV_ID, b'\x01', expected=1)
            for _ in range(10):
                cmd(m.CMD_NEW_DEV_ID)
            cmd(m.CMD_FACTORY_RESET)
            cmd(m.CMD_STATUS, response=m.STATUS_RESP)
            cmd(0x7E, expected=4)
            cmd(m.CMD_EXIT)
    finally:
        # Recovery restores the fixture's complete NVS snapshot before rollback.
        port = u.wait_for_by_id('ZLRS', 45, exclude='MCUBOOT', serial=args.serial, exact_serial=True)
        assert port
        rc, output = u.run_mgmt_subcmd_retry(port, 'recovery')
        print(output, flush=True)
        assert rc == 0
        recovery = u.wait_for_by_id('MCUBOOT', 30, serial=args.serial, exact_serial=True)
        assert recovery
        for attempt in range(3):
            r = subprocess.run([u.find_client(), '--port', recovery, '--baudrate', '115200', 'os', 'reset'], timeout=20)
            if r.returncode == 0 or not Path(recovery).exists():
                break
            time.sleep(1)
        else:
            r.check_returncode()
        assert u.wait_for_by_id('ZLRS', 45, exclude='MCUBOOT', serial=args.serial, exact_serial=True)
        rc, out = u.run_mgmt_subcmd_retry(port, 'image-hash')
        assert rc == 0 and u.parse_hex_hash(out) == previous, (rc, out)
        restored = run('status')
        # Acquisition changes quality/state; saved configuration and IDs must match.
        for label in ('Device ID', 'Peer ID', 'Region', 'TX ceiling', 'Data UART', 'Profile'):
            before = next((line.strip() for line in original.splitlines() if label in line))
            after = next((line.strip() for line in restored.splitlines() if label in line))
            assert before == after, (before, after)
        print('Original NVS and firmware restored.', flush=True)
    print('All management commands passed.', flush=True)


if __name__ == '__main__':
    main()
