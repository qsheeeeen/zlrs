#!/usr/bin/env python3
#
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
#
# Bench self-test: one command checks everything that does not need the radio:
# three CDCs, enter config mode on the config port, config protocol reads and writes
# (including PBKDF2 into NVS), then re-enter to confirm NVS persistence. Nothing radio.
#
# Usage:
#   python3 tools/zlrs-bringup.py                # finds ports by-id
#   python3 tools/zlrs-bringup.py --config /dev/ttyACM2
#   python3 tools/zlrs-bringup.py --self-test    # check the logic without a device
#
# The config port is the third CDC in application mode (...-if04).

import argparse
import glob
import subprocess
import sys
import time
from pathlib import Path

BY_ID_DIR = "/dev/serial/by-id"
APP_HINT = "ZLRS"
MCUBOOT_HINT = "MCUBOOT"
DEFAULT_BAUD = 115200


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def find_by_id(hint: str, exclude: str | None = None) -> list[str]:
    out = []
    for path in sorted(glob.glob(f"{BY_ID_DIR}/*")):
        name = Path(path).name.upper()
        if hint.upper() not in name:
            continue
        if exclude and exclude.upper() in name:
            continue
        out.append(path)
    return out


def app_ports() -> list[str]:
    return find_by_id(APP_HINT, exclude=MCUBOOT_HINT)


def pick_port(ports: list[str], iface: str) -> str | None:
    """CDCs are recognised by USB interface number: if00 CRSF, if02 data, if04 config, if06 console."""
    for path in ports:
        if Path(path).name.endswith(iface):
            return path
    return None


def run_config(*args: str) -> tuple[int, str]:
    """
    Run one config command with a generous timeout: set-phrase does 100000 PBKDF2 rounds on the
    device, which is tens of seconds on an M33; a 1 s default would call that "no answer".
    """
    tool = repo_root() / "tools" / "zlrs-config.py"
    proc = subprocess.run([sys.executable, str(tool), "--timeout", "120", *args], check=False,
                          capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def enter_config_mode(data_port: str, tries: int = 8) -> bool:
    """Probe with the config tool: it sends enter-config-mode first, an answer means we are in."""
    for attempt in range(1, tries + 1):
        rc, out = run_config("--port", data_port, "status")
        if rc == 0 and "role=" in out:
            print(f"    config mode entered (attempt {attempt})")
            print(f"    {out}")
            return True
        time.sleep(0.5)
    return False


def self_test() -> int:
    problems = []

    fake = [
        "/dev/serial/by-id/usb-zlrs_ZLRS_1234-if00",
        "/dev/serial/by-id/usb-zlrs_ZLRS_1234-if02",
        "/dev/serial/by-id/usb-zlrs_ZLRS_1234-if04",
        "/dev/serial/by-id/usb-zlrs_ZLRS_MCUBOOT_1234-if00",
    ]
    original = glob.glob
    glob.glob = lambda pattern: list(fake)  # type: ignore[assignment]
    try:
        ports = app_ports()
        if len(ports) != 3:
            problems.append(f"application port detection is wrong: {ports}")
        if pick_port(ports, "-if02") != fake[1]:
            problems.append("data port detection is wrong")
        if pick_port(ports, "-if04") != fake[2]:
            problems.append("config port detection is wrong")
    finally:
        glob.glob = original  # type: ignore[assignment]

    # The config tool has to support the formal enter-config-mode entry
    tool = repo_root() / "tools" / "zlrs-config.py"
    if "CMD_ENTER" not in tool.read_text(encoding="utf-8"):
        problems.append("the config tool has no CMD_ENTER")

    for line in problems:
        print(f"  FAIL {line}")
    print(f"self-test: {'ok' if not problems else str(len(problems)) + ' failure(s)'}")
    return 0 if not problems else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="bench self-test (the non-radio half)")
    parser.add_argument("--data", help="data port device (defaults to by-id ...-if02)")
    parser.add_argument("--config", help="config port device (defaults to by-id ...-if04)")
    parser.add_argument("--console", help="console port of a debug build (...-if06)")
    parser.add_argument("--phrase", default="zlrs bring-up phrase",
                        help="passphrase for set-phrase")
    parser.add_argument("--self-test", action="store_true", help="check the logic without a device")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    ports = app_ports()
    if not ports:
        print(f"no application ports ({BY_ID_DIR} has no {APP_HINT}): is the board plugged in?", 
              file=sys.stderr)
        return 1

    data = args.data or pick_port(ports, "-if02")
    config = args.config or pick_port(ports, "-if04")
    console = args.console or pick_port(ports, "-if06")
    if data is None:
        print(f"no data port (...-if02): {ports}", file=sys.stderr)
        return 1
    if config is None:
        print(f"no config port (...-if04): {ports}", file=sys.stderr)
        return 1

    print("1) ports")
    for p in ports:
        print(f"    {p}")
    if console is not None:
        print(f"    debug console: {console}")

    print("2) enter config mode on the config port")
    if not enter_config_mode(config):
        print("    cannot enter. Check whether another program holds the config port "
              "and whether the firmware carries the config protocol", file=sys.stderr)
        return 1

    print("3) config protocol")
    rc, out = run_config("--port", config, "set-phrase", args.phrase)
    print(f"    set-phrase: rc={rc} {out}")
    if rc != 0:
        return 1

    rc, out = run_config("--port", config, "set-failsafe", "@default")
    print(f"    set-failsafe: rc={rc} {out}")
    if rc != 0:
        return 1

    rc, out = run_config("--port", config, "status")
    print(f"    status: {out}")
    if rc != 0 or "key=yes" not in out:
        print("    the key flag is still no after writing the phrase", file=sys.stderr)
        return 1

    rc, out = run_config("--port", config, "exit")
    print(f"    exit: rc={rc} {out}")
    time.sleep(0.3)

    print("4) NVS persistence (enter config mode again)")
    if not enter_config_mode(config):
        print("    entering config mode a second time failed", file=sys.stderr)
        return 1

    rc, out = run_config("--port", config, "status")
    print(f"    status: {out}")
    if rc != 0 or "key=yes" not in out:
        print("    the key flag is not yes after the reboot: NVS did not persist", file=sys.stderr)
        return 1

    run_config("--port", data, "exit")

    print("self-test passed: ports, config mode, config protocol and NVS persistence all line up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
