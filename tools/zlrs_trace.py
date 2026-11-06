# SPDX-License-Identifier: Apache-2.0
"""Bounded Zephyr tracing control over MCUmgr serial, without a device shell."""

import json
from pathlib import Path

from zlrs_cli import bounded_int, positive_seconds
import zlrs_mgmt as mgmt
from zlrs_smp import request

GROUP = 64
OPERATIONS = {"start": 0, "stop": 1, "save": 2, "discard": 3, "arm": 4}
STATES = ("idle", "capturing", "frozen", "saving", "saved")


def configure_parser(parser):
    parser.add_argument("action", choices=("status", *OPERATIONS))
    parser.add_argument("duration_ms", nargs="?", type=bounded_int(10, 5000))
    targets = parser.add_mutually_exclusive_group()
    targets.add_argument("--port", help="MCUmgr serial port (management CDC if02)")
    targets.add_argument("--serial", help="module USB serial-number fragment")
    parser.add_argument("--baud", type=bounded_int(1, 4000000), default=115200)
    parser.add_argument("--timeout", type=positive_seconds, default=10.0)
    parser.set_defaults(all=False)


def main(args, parser):
    if args.duration_ms is not None and args.action != "start":
        parser.error("duration_ms is only valid with start")
    if args.port:
        port = args.port
    else:
        management = Path(mgmt.pick_targets(args, parser)[0])
        port = str(management)
    fields = None if args.action == "status" else {"op": OPERATIONS[args.action]}
    if args.duration_ms is not None:
        fields["ms"] = args.duration_ms
    try:
        result = request(port, GROUP, fields, timeout=args.timeout, baud=args.baud)
    except ImportError as error:
        raise ValueError("smpclient with serial support is required; install smpmgr") from error
    if result["result"] != 0:
        raise ValueError(f"trace {args.action} failed ({result['result']})")
    result["state"] = STATES[result["state"]]
    print(json.dumps(result))
    return 0
