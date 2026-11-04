# SPDX-License-Identifier: Apache-2.0
"""The architecture gate must reject violations and failed scans alike."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from support import REPO


class GateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        shutil.copyfile(REPO / "README.md", self.root / "README.md")
        for name in ("app", "boards", "core", "include", "tools", "doc", "tests"):
            shutil.copytree(REPO / name, self.root / name,
                            ignore=shutil.ignore_patterns("build*", "__pycache__"))

    def run_gate(self, **env):
        return subprocess.run(["bash", str(self.root / "tests/gates.sh")],
                              env={**os.environ, **env}, capture_output=True, text=True,
                              timeout=10)

    def test_clean_source_archive_passes_without_git(self):
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failed_search_is_not_a_clean_tree(self):
        binary = self.root / "bin"
        binary.mkdir()
        search = binary / "rg"
        search.write_text("#!/bin/sh\nexit 2\n")
        search.chmod(0o755)
        result = self.run_gate(PATH=str(binary) + os.pathsep + os.environ["PATH"])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("scan failed", result.stdout)

    def test_missing_boundary_source_fails(self):
        (self.root / "app/src/persist.h").unlink()
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("scan failed", result.stdout)

    def test_quoted_zephyr_include_in_core_fails(self):
        (self.root / "core/forbidden.h").write_text('# include "zephyr/kernel.h"\n')
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pulled in a Zephyr header", result.stdout)

    def test_new_production_config_is_checked_before_git_add(self):
        (self.root / "app/production.conf").write_text("CONFIG_ZLRS_APP_DEBUG=y\n")
        result = self.run_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("a shipped configuration carries a debug option", result.stdout)
