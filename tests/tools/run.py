# SPDX-License-Identifier: Apache-2.0
"""Run all tool tests, or the tests matching one tool module."""

import argparse
from pathlib import Path
import sys
import unittest

DIRECTORY = Path(__file__).resolve().parent
sys.path.insert(0, str(DIRECTORY))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", choices=[path.stem[5:] for path in
                                           sorted(DIRECTORY.glob("test_*.py"))])
    args = parser.parse_args()
    pattern = f"test_{args.module}.py" if args.module else "test_*.py"
    suite = unittest.defaultTestLoader.discover(str(DIRECTORY), pattern=pattern)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
