#!/usr/bin/env python3
# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
"""The public entry point for ZLRS device tools."""

import argparse

from zlrs_cli import HelpFormatter, run
import zlrs_mgmt as mgmt
import zlrs_recovery as recovery
import zlrs_update as update
import zlrs_throughput as throughput


def self_test():
    failures = 0
    for name, module in (("management", mgmt), ("recovery", recovery), ("update", update)):
        print(f"\n{name}:")
        failures += module.self_test() != 0
    return 1 if failures else 0


def build_parser():
    parser = argparse.ArgumentParser(
        description="ZLRS device management, firmware updates and link measurements.",
        formatter_class=HelpFormatter,
        epilog="""Examples:
  %(prog)s list
  %(prog)s status --serial <serial>
  %(prog)s set-role rx --serial <serial>
  %(prog)s set-phrase 'my binding phrase' --serial <serial>
  %(prog)s update --serial <serial> --image <signed.bin>
  %(prog)s recovery --serial <serial>
  %(prog)s throughput --tx-port <TX-serial> --rx-port <RX-serial>

Use COMMAND --help for options. A single connected module is selected automatically
for management commands. With several modules, use --serial or --port.
""")
    parser.add_argument("--self-test", action="store_true", help="check all device tools without hardware")
    sub = parser.add_subparsers(dest="command", title="commands", metavar="COMMAND")

    options = argparse.ArgumentParser(add_help=False)
    mgmt.add_options(options, include_list=False)
    commands = mgmt.add_commands(sub, parents=[options])
    for child in commands.values():
        child.set_defaults(handler=mgmt.main, command_parser=child)
    listing = sub.add_parser("list", help="list connected ZLRS management ports",
                             formatter_class=HelpFormatter)
    listing.set_defaults(handler=mgmt.main, list=True, self_test=False, command_parser=listing)
    upgrade = sub.add_parser("update", help="upload, verify and confirm a signed firmware image",
                             description="Upgrade ZLRS through MCUboot recovery.",
                             formatter_class=HelpFormatter)
    update.configure_parser(upgrade)
    upgrade.set_defaults(handler=update.main, command_parser=upgrade)
    measure = sub.add_parser("throughput", help="measure verified bidirectional data throughput",
                             description="Measure verified bidirectional ZLRS data throughput.",
                             formatter_class=HelpFormatter)
    throughput.configure_parser(measure)
    measure.set_defaults(handler=throughput.main, command_parser=measure)
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    if args.command is None:
        if args.self_test:
            return self_test()
        parser.print_help()
        return 2
    # Each module consumes the namespace parsed by its subcommand; no second parse or subprocess.
    if args.command == "recovery" and args.self_test:
        return recovery.self_test()
    return args.handler(args=args, parser=args.command_parser)


if __name__ == "__main__":
    raise SystemExit(run(main))
