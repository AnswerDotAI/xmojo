#!/usr/bin/env python
# Copyright (c) 2026, xmojo contributors.
# Licensed under the Apache License v2.0 with LLVM Exceptions.

import sys, unittest
from pathlib import Path
from conkernelclient.ops import iopub_streams, run_kernel
from gpu_story import CELLS


class GPUKernelStory(unittest.IsolatedAsyncioTestCase):
    async def test_device_state_and_diagnostics_across_jupyter_cells(self):
        binary = str(Path(sys.argv[1]).resolve())
        async with run_kernel("xmojo", argv=[binary, "kernel", "-f", "{connection_file}"]) as (_, client):
            output = ""
            for cell in CELLS:
                reply, published = await client.exec_drain(cell, timeout=180)
                self.assertEqual(reply["content"]["status"], "ok", reply["content"])
                output += "".join(m["content"]["text"] for m in iopub_streams(published, "stdout"))
            self.assertEqual(output, "allocated\nGPU result verified\n")

            # A failed cell must neither execute its fill nor poison GPU state.
            reply, _ = await client.exec_drain(
                "buffer.enqueue_fill(99)\ncontext.enqueue_function[missing_kernel](buffer, grid_dim=1, block_dim=16)\n",
                timeout=180)
            self.assertEqual(reply["content"]["status"], "error")
            self.assertIn("missing_kernel", reply["content"]["evalue"])
            reply, published = await client.exec_drain(CELLS[-1], timeout=180)
            self.assertEqual(reply["content"]["status"], "ok", reply["content"])
            self.assertEqual("".join(m["content"]["text"] for m in iopub_streams(published, "stdout")),
                             "GPU result verified\n")


if __name__ == "__main__": unittest.main(argv=sys.argv[:1])
