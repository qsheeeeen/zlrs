# Copyright (c) 2026 zlrs contributors
# SPDX-License-Identifier: Apache-2.0
"""Small, standard-library helpers shared by the operator tools."""

import argparse
import math
import os
import sys


class HelpFormatter(argparse.ArgumentDefaultsHelpFormatter,
                    argparse.RawDescriptionHelpFormatter):
    def __init__(self, prog):
        super().__init__(prog, max_help_position=28, width=100)

    def _get_help_string(self, action):
        if action.default is None or isinstance(action.default, bool):
            return action.help
        return super()._get_help_string(action)


def styled(text, code, file=None):
    stream = sys.stdout if file is None else file
    if stream.isatty() and "NO_COLOR" not in os.environ and os.environ.get("TERM") != "dumb":
        return f"\033[{code}m{text}\033[0m"
    return text


def fields(rows):
    """Aligned labels, with stable plain text for redirected output."""
    rows = list(rows)
    width = max((len(label) for label, _ in rows), default=0)
    return "\n".join(f"  {label:<{width}} : {value}" for label, value in rows)


def heading(title):
    print(styled(title, "1;36"))
    print(styled("-" * min(len(title), 72), "2"))


def notice(kind, message, file=None):
    stream = sys.stdout if file is None else file
    colors = {"ok": "32", "info": "36", "warn": "33", "error": "31"}
    tag = styled(f"[{kind.upper()}]", colors[kind], stream)
    print(f"{tag} {message}", file=stream, flush=True)


def step(number, total, message):
    print(f"\n{styled(f'[{number}/{total}]', '1;36')} {message}", flush=True)


def bounded_int(low, high):
    def parse(text):
        try:
            value = int(text)
        except ValueError:
            raise argparse.ArgumentTypeError("expected an integer") from None
        if not low <= value <= high:
            raise argparse.ArgumentTypeError(f"must be between {low} and {high}")
        return value
    return parse


def positive_seconds(text):
    try:
        value = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError("expected seconds") from None
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return value


def run(main):
    """Keep expected operator errors concise; preserve programming errors for debugging."""
    try:
        return main()
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        return 130
    except (OSError, ValueError, TimeoutError) as exc:
        notice("error", str(exc), file=sys.stderr)
        return 1
