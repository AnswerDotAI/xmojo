#!/usr/bin/env python
# Copyright (c) 2026, xmojo contributors.
# Licensed under the Apache License v2.0 with LLVM Exceptions.

import subprocess, sys, unittest
from pathlib import Path
from gpu_story import CELLS


class GPUReplStory(unittest.TestCase):
    def test_device_state_survives_repl_cells(self):
        binary = str(Path(sys.argv[1]).resolve())
        source = "\n".join(CELLS) + "\n:quit\n\n"
        result = subprocess.run([binary], input=source, text=True,
                                capture_output=True, timeout=180)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("error:", result.stderr)
        self.assertEqual(result.stdout.count("allocated"), 1)
        self.assertEqual(result.stdout.count("GPU result verified"), 1)


if __name__ == "__main__": unittest.main(argv=sys.argv[:1])
