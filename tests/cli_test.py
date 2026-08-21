#!/usr/bin/env python

# Copyright (c) 2026, xmojo contributors.
# Licensed under the Apache License v2.0 with LLVM Exceptions.

import subprocess, sys, tempfile, unittest
from pathlib import Path


class CLIStory(unittest.TestCase):
    def run_ok(self, *args):
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def test_one_command_supports_interactive_and_compiler_workflows(self):
        xmojo, version = str(Path(sys.argv[1]).resolve()), sys.argv[2]

        result = self.run_ok(xmojo, "--version")
        self.assertEqual(result.stdout, f"xmojo {version}\n")

        result = self.run_ok(xmojo, "-e", 'print("interactive")')
        self.assertEqual(result.stdout, "interactive\n")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, executable = root / "hello.mojo", root / "hello"
            source.write_text('def main():\n    print("compiled")\n')
            self.run_ok(xmojo, "build", source, "-o", executable)
            result = self.run_ok(executable)
            self.assertEqual(result.stdout, "compiled\n")

            package, artifact = root / "story", root / "story.mojoc"
            package.mkdir()
            (package / "__init__.mojo").write_text("def answer() -> Int:\n    return 42\n")
            self.run_ok(xmojo, "precompile", package, "-o", artifact)
            self.assertGreater(artifact.stat().st_size, 0)


if __name__ == "__main__": unittest.main(argv=sys.argv[:1])
