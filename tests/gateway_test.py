#!/usr/bin/env python

# Copyright (c) 2026, xmojo contributors.
# Licensed under the Apache License v2.0 with LLVM Exceptions.

import json, tempfile, unittest
from pathlib import Path

import httpx
from jupyasyncclient import JupyAsyncKernelClient
from jupywire.route import JmsgQueues
from rustygate.tools import start_gateway


class GatewayStory(unittest.IsolatedAsyncioTestCase):
    async def test_stop_restarts_native_code_without_losing_its_notebook_binding(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cells = [dict(id=cid, cell_type="code", source=source, metadata={}, outputs=[], execution_count=None) for cid, source in [
                ("state", "var kept = 41\n"), ("busy", 'print("native-running")\nwhile True:\n  pass\n')]]
            (root/"stop.ipynb").write_text(json.dumps(dict(nbformat=4, nbformat_minor=5, metadata={}, cells=cells)))
            gateway = start_gateway(["rustygate", "--root", str(root), "--settings-file", str(root/"settings.json")])
            try:
                with httpx.Client(base_url=gateway.url, timeout=90) as http:
                    response = http.post("/api/kernels", json=dict(path="stop.ipynb", name="xmojo"))
                    response.raise_for_status()
                    before = response.json()
                    kid = before["id"]
                    try:
                        async with await JupyAsyncKernelClient.connect(gateway.url, kid, default_timeout=60) as client:
                            info = (await client.kernel_info())["content"]
                            self.assertIn("interrupt requires restart", info["supported_features"])
                            self.assertEqual((await client.reply(cell_id="state"))["content"]["status"], "ok")
                            running = client.run(cell_id="busy")
                            stdout = ""
                            async for message in running:
                                if message["msg_type"] == "stream" and message["content"]["name"] == "stdout":
                                    stdout += message["content"]["text"]
                                    if "native-running\n" in stdout: break
                            self.assertIn("native-running\n", stdout)

                            # Solveit's Stop is a websocket control request; restart must not block on native code.
                            messages = JmsgQueues(client, queues=("control",))
                            stop = client.session.msg("interrupt_request", {})
                            client.send(stop, "control")
                            stopped = await messages.jmsg_for("interrupt_reply", queue="control", timeout=60,
                                pred=lambda m: m["parent_header"].get("msg_id") == stop["header"]["msg_id"])
                            self.assertEqual(stopped["content"]["status"], "ok")
                            with self.assertRaises(ConnectionError):
                                async for _ in running: pass

                            after = await client.model()
                            self.assertEqual((after["id"], after["path"]), (kid, "stop.ipynb"))
                            self.assertNotEqual(after["pid"], before["pid"])
                            missing = (await client.reply("print(kept)\n", stop_on_error=False))["content"]
                            self.assertEqual(missing["status"], "error")
                            self.assertIn("kept", missing["evalue"])
                            outputs = await client.exec_outs("var kept = 42\nkept\n")
                            self.assertEqual(outputs[0]["data"]["text/plain"], "Int(42)")
                    finally: http.delete(f"/api/kernels/{kid}").raise_for_status()
            finally: gateway.stop()


if __name__ == "__main__": unittest.main()
