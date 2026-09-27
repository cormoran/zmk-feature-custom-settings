"""Exercise the production allocator independently of Zephyr and its toolchain."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class PoolTests(unittest.TestCase):
    def test_randomized_pool_invariants(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "pool-test"
            subprocess.run(
                [
                    os.environ.get("CC", "cc"),
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=undefined",
                    "-I",
                    str(ROOT / "include"),
                    str(ROOT / "src/custom_settings_allocator.c"),
                    str(ROOT / "tests/host/pool_test.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)
