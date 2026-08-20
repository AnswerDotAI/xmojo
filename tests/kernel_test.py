#!/usr/bin/env python

# Copyright (c) 2026, xmojo contributors.
# Licensed under the Apache License v2.0 with LLVM Exceptions.

import sys, unittest
from pathlib import Path

from conkernelclient.ops import iopub_msgs, iopub_streams, run_kernel


class KernelStory(unittest.IsolatedAsyncioTestCase):
    @staticmethod
    def stream(published, name): return "".join(m["content"]["text"] for m in iopub_streams(published, name))

    async def test_real_kernel_runs_a_multi_cell_story(self):
        binary = str(Path(sys.argv[1]).resolve())
        async with run_kernel("xmojo", argv=[binary, "-f", "{connection_file}"]) as (_, client):
            reply, published = await client.exec_drain(
                'def answer() -> Int:\n  return 42\n\nprint("ready")\n', timeout=60)
            reply = reply["content"]
            self.assertEqual(reply["status"], "ok")
            self.assertEqual(self.stream(published, "stdout"), "ready\n")

            reply, published = await client.exec_drain("print(answer())\n", timeout=60)
            reply = reply["content"]
            self.assertEqual(reply["status"], "ok")
            self.assertEqual(self.stream(published, "stdout"), "42\n")

            reply, published = await client.exec_drain(
                'from std.sys import stderr\nprint("warning", file=stderr)\n', timeout=60)
            reply = reply["content"]
            self.assertEqual(reply["status"], "ok")
            self.assertEqual(self.stream(published, "stderr"), "warning\n")

            reply, published = await client.exec_drain("print(unknown_name)\n", timeout=60)
            reply = reply["content"]
            self.assertEqual(reply["status"], "error")
            self.assertEqual(reply["ename"], "MojoError")
            errors = iopub_msgs(published, "error")
            self.assertEqual(len(errors), 1)
            self.assertIn("unknown_name", errors[0]["content"]["evalue"])


if __name__ == "__main__": unittest.main(argv=sys.argv[:1])
