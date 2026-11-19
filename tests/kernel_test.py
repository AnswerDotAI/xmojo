#!/usr/bin/env python

# Copyright (c) 2026, xmojo contributors.
# Licensed under the Apache License v2.0 with LLVM Exceptions.

import asyncio, sys, unittest
from pathlib import Path

from conkernelclient.ops import iopub_msgs, iopub_streams, run_kernel


class KernelStory(unittest.IsolatedAsyncioTestCase):
    @staticmethod
    def stream(published, name): return "".join(m["content"]["text"] for m in iopub_streams(published, name))

    async def test_real_kernel_runs_a_multi_cell_story(self):
        binary = str(Path(sys.argv[1]).resolve())
        async with run_kernel("xmojo", argv=[binary, "kernel", "-f", "{connection_file}"]) as (_, client):
            reply, published = await client.exec_drain(
                'def answer() -> Int:\n  """Return the answer used by this kernel story."""\n  return 42\n\n'
                'def twice(value: Int) -> Int:\n  """Double an integer."""\n  return value * 2\n\n'
                'def add[offset: Int](a: Int, b: Int) -> Int:\n  return a + b + offset\n\nprint("ready")\n', timeout=60)
            reply = reply["content"]
            self.assertEqual(reply["status"], "ok")
            self.assertEqual(self.stream(published, "stdout"), "ready\n")

            inspection = (await client.cmd.inspect(code="twice()", cursor_pos=6, detail_level=0))["content"]
            self.assertIn("application/json", inspection["data"], inspection)
            signatures = inspection["data"]["application/json"]
            self.assertEqual(signatures["activeParameter"], 0)
            self.assertIn("value: Int", signatures["signatures"][signatures["activeSignature"]]["label"])

            reply, _ = await client.exec_drain('var x = 1\nvar y = 1\nvar owned = String("kept")\n', timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")

            completion = (await client.cmd.complete(code="# é\nans", cursor_pos=7))["content"]
            self.assertEqual(completion["status"], "ok")
            self.assertIn("answer", completion["matches"])
            self.assertEqual((completion["cursor_start"], completion["cursor_end"]), (4, 7))
            completion = (await client.cmd.complete(code="own", cursor_pos=3))["content"]
            owned = next(item for item in completion["metadata"]["_jupyter_types_experimental"] if item["text"] == "owned")
            self.assertEqual(owned["signature"], "var owned: String")

            for level in (0, 1):
                inspection = (await client.cmd.inspect(code="# é\nanswer()", cursor_pos=7, detail_level=level))["content"]
                self.assertTrue(inspection["found"])
                self.assertIn("def answer", inspection["data"]["text/markdown"])
                self.assertEqual("return 42" in inspection["data"]["text/markdown"], level == 1)

            for query in ("?answer", "answer?", "??answer", "answer??"):
                reply, published = await client.exec_drain(query, timeout=60)
                self.assertEqual(reply["content"]["status"], "ok")
                markdown = iopub_msgs(published, "display_data")[0]["content"]["data"]["text/markdown"]
                self.assertIn("Return the answer used", markdown)
                self.assertEqual("return 42" in markdown, "??" in query)

            call = "# é\nadd[1](2, )"
            inspection = (await client.cmd.inspect(code=call, cursor_pos=len(call)-1, detail_level=0))["content"]
            self.assertIn("application/json", inspection["data"], inspection)
            signatures = inspection["data"]["application/json"]
            self.assertEqual(signatures["activeParameter"], 1)
            signature = signatures["signatures"][signatures["activeSignature"]]
            self.assertIn("[offset: Int]", signature["label"])
            self.assertIn("-> Int", signature["label"])
            start, end = signature["parameters"][1]["label"]
            self.assertEqual(signature["label"][start:end], "b: Int")

            incomplete = (await client.cmd.is_complete(code="def unfinished():\n"))["content"]
            self.assertEqual(incomplete["status"], "incomplete")
            self.assertEqual(incomplete["indent"], "  ")
            for code, status in [("def finished() -> Int:\n  return 1\n", "complete"), ("print((1 + 2]\n", "invalid")]:
                self.assertEqual((await client.cmd.is_complete(code=code))["content"]["status"], status)

            reply, published = await client.exec_drain("answer()\n", timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")
            results = iopub_msgs(published, "execute_result")
            self.assertEqual(len(results), 1)
            self.assertEqual(results[0]["content"]["data"]["text/plain"], "Int(42)")

            reply, published = await client.exec_drain(
                'from xmojo import HTMLRepr, display\n\n@fieldwise_init\nstruct HTML(HTMLRepr):\n'
                '  var value: Int\n\n  def _repr_html_(self) -> String:\n    return String("<i>", self.value, "</i>")\n\n'
                'display(HTML(7))\n', timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")
            displays = iopub_msgs(published, "display_data")
            self.assertEqual(len(displays), 1)
            self.assertEqual(displays[0]["content"]["data"]["text/html"], "<i>7</i>")

            reply, published = await client.exec_drain('print(99)\ndisplay(HTML(8))\nanswer()\n', silent=True, timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")
            self.assertFalse([m for m in published if m["msg_type"] in ("stream", "display_data", "execute_result")])

            reply, published = await client.exec_drain("print(answer()) # why?\n", timeout=60)
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

            # An AI prompt parks normal code; its priority tool uses the same native session before release.
            held_id = client.new_msg_id()
            held = client.run("This is an AI prompt, not Mojo code.", msg_id=held_id, metadata={"hold": True}, timeout=60)
            async for message in held:
                if message["msg_type"] == "execute_input": break
            queued = client.run("print(y)\n", timeout=60)
            queued_start = asyncio.create_task(anext(queued))
            reply, _ = await client.exec_drain("y += 41\n", metadata={"priority": 1}, timeout=60)
            self.assertEqual(reply["content"]["status"], "ok")
            self.assertFalse(queued_start.done())
            self.assertTrue((await client.ctl.release(msg_id=held_id, timeout=60))["content"]["found"])
            held_messages = [message async for message in held]
            held_reply = next(m for m in held_messages if m["msg_type"] == "execute_reply")
            self.assertEqual(held_reply["content"]["status"], "ok")
            queued_messages = [await queued_start] + [message async for message in queued]
            self.assertEqual(self.stream(queued_messages, "stdout"), "42\n")


if __name__ == "__main__": unittest.main(argv=sys.argv[:1])
