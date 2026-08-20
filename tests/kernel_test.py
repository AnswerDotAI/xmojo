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

            reply, _ = await client.exec_drain("var x = 1\nvar y = 1\n", timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")

            completion = (await client.cmd.complete(code="ans", cursor_pos=3))["content"]
            self.assertEqual(completion["status"], "ok")
            self.assertIn("answer", completion["matches"])
            self.assertIn("_jupyter_types_experimental", completion["metadata"])

            inspection = (await client.cmd.inspect(code="answer()", cursor_pos=3, detail_level=0))["content"]
            self.assertTrue(inspection["found"])
            self.assertIn("def answer", inspection["data"]["text/markdown"])

            incomplete = (await client.cmd.is_complete(code="def unfinished():\n"))["content"]
            self.assertEqual(incomplete["status"], "incomplete")
            self.assertEqual(incomplete["indent"], "  ")

            reply, published = await client.exec_drain("answer()\n", timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")
            results = iopub_msgs(published, "execute_result")
            self.assertEqual(len(results), 1)
            self.assertEqual(results[0]["content"]["data"]["text/plain"], "Int(42)")

            reply, published = await client.exec_drain(
                "from xmojo import display\ndisplay(7)\n", timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")
            displays = iopub_msgs(published, "display_data")
            self.assertEqual(len(displays), 1)
            self.assertEqual(displays[0]["content"]["data"]["text/plain"], "Int(7)")

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

            reply, published = await client.exec_drain(
                'def survives_raise() -> Int:\n  return 123\n\nx += 1\nprint("before raise")\nraise Error("kaboom")\n',
                timeout=60)
            reply = reply["content"]
            self.assertEqual(reply["status"], "error")
            self.assertEqual(reply["ename"], "MojoError")
            self.assertEqual(reply["evalue"], "kaboom")
            self.assertEqual(self.stream(published, "stdout"), "before raise\n")
            self.assertIn("kaboom", reply["traceback"][0])

            reply, published = await client.exec_drain("print(x, y)\n", timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")
            self.assertEqual(self.stream(published, "stdout"), "2 1\n")

            reply, published = await client.exec_drain("survives_raise()\n", timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")
            results = iopub_msgs(published, "execute_result")
            self.assertEqual(len(results), 1)
            self.assertEqual(results[0]["content"]["data"]["text/plain"], "Int(123)")


if __name__ == "__main__": unittest.main(argv=sys.argv[:1])
