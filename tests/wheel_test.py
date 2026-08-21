#!/usr/bin/env python

# Copyright (c) 2026, xmojo contributors.
# Licensed under the Apache License v2.0 with LLVM Exceptions.

import subprocess, sys, tempfile, unittest
from pathlib import Path


class WheelStory(unittest.TestCase):
    def run_ok(self, *args):
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_clean_install_runs_the_existing_cli_and_kernel_stories(self):
        wheel, cli_story, kernel_story = map(lambda path: str(Path(path).resolve()), sys.argv[1:4])
        version = sys.argv[4]
        with tempfile.TemporaryDirectory() as directory:
            environment = Path(directory) / "venv"
            self.run_ok("uv", "venv", "--python", sys.executable, environment)
            python, xmojo = environment / "bin" / "python", environment / "bin" / "xmojo"
            self.run_ok("uv", "pip", "install", "--no-index", "--python", python, wheel)

            self.assertTrue(xmojo.exists())
            self.assertFalse((environment / "bin" / "mojoorc").exists())
            self.run_ok(python, cli_story, xmojo, version)
            self.run_ok(sys.executable, kernel_story, xmojo)


if __name__ == "__main__": unittest.main(argv=sys.argv[:1])
