"""Protocol and pixel protection regression tests; no GPU or cloud needed."""
import io
import json
from pathlib import Path
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from comfyui_client import ComfyClient, ComfyError, apply_bindings, build_workflow, local_composite


class Server:
    def __init__(self, respond):
        self.requests = []
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.handle_request()

            def do_POST(self):
                self.handle_request()

            def handle_request(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                fixture.requests.append((self.command, self.path, body))
                status, result = respond(self.path, body)
                data = result if isinstance(result, bytes) else json.dumps(result).encode()
                self.send_response(status)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.client = ComfyClient(f"http://127.0.0.1:{self.server.server_port}", timeout=2)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class ComfyTests(unittest.TestCase):
    def config(self, **changes):
        return {"model": "edit.safetensors", "text_encoder": "clip.safetensors", "vae": "vae.safetensors",
                "width": 512, "height": 768, "steps": 20, **changes}

    def test_reference_order_and_text_only(self):
        graph = build_workflow(self.config(), "图1保留人物，图2参考背景", ["a.png", "b.png", "c.png"])
        self.assertEqual(graph["8"]["inputs"]["image2"], ["21", 0])
        self.assertEqual(graph["22"]["inputs"]["image"], "c.png")
        self.assertEqual(graph["6"]["inputs"]["height"], 768)
        self.assertNotIn("image1", build_workflow(self.config(), "水彩画", [])["8"]["inputs"])
        with self.assertRaisesRegex(ComfyError, "三张"):
            build_workflow(self.config(), "p", ["a", "b", "c", "d"])

    def test_checkpoint_edit_and_mask_direction(self):
        config = self.config(family="checkpoint")
        graph = build_workflow(config, "p", ["photo.png"], "mask.png")
        self.assertEqual(graph["6"]["class_type"], "VAEEncodeForInpaint")
        self.assertEqual(graph["6"]["inputs"]["mask"], ["22", 1])
        self.assertEqual(graph["10"]["inputs"]["denoise"], 0.65)
        with self.assertRaises(ComfyError):
            build_workflow(config, "p", ["a", "b"])
        with self.assertRaises(ComfyError):
            build_workflow(self.config(width=513), "p", [])

    def test_bindings_do_not_mutate_template(self):
        template = {"1": {"class_type": "LoadImage", "inputs": {"image": "old"}},
                    "2": {"class_type": "Text", "inputs": {"text": "old"}}}
        bindings = {"image2": "1.image", "prompt": "2.text", "output": "7"}
        result = apply_bindings(template, bindings, {"image2": "new.png", "prompt": "中文"})
        self.assertEqual(result["1"]["inputs"]["image"], "new.png")
        self.assertEqual(template["1"]["inputs"]["image"], "old")
        with self.assertRaises(ComfyError):
            apply_bindings(template, bindings, {"prompt": "p"})
        with self.assertRaises(ComfyError):
            apply_bindings(template, {"prompt": "2.missing", "output": "7"}, {"prompt": "p"})

    def test_health_needs_no_model_and_never_generates(self):
        server = Server(lambda path, body: (200, {"system": {"comfyui_version": "test"}}))
        self.addCleanup(server.close)
        self.assertEqual(server.client.check()["system"]["comfyui_version"], "test")
        self.assertEqual(server.requests, [("GET", "/system_stats", b"")])

    def test_bad_server_and_error_detail(self):
        server = Server(lambda path, body: (200, b"<html>not ComfyUI</html>"))
        self.addCleanup(server.close)
        with self.assertRaisesRegex(ComfyError, "JSON"):
            server.client.check()
        with self.assertRaises(ComfyError):
            ComfyClient("http://user:secret@127.0.0.1:8188")

    def test_preflight_rejects_missing_models_without_queueing(self):
        server = Server(lambda path, body: (200, {"UNETLoader": {"input": {"required": {"unet_name": [["installed.safetensors"]]}}}}))
        self.addCleanup(server.close)
        graph = {"1": {"class_type": "UNETLoader", "inputs": {"unet_name": "missing.safetensors"}}}
        with self.assertRaisesRegex(ComfyError, "不可用"):
            server.client.execute(graph, ["7"])
        self.assertFalse(any(path == "/prompt" for _, path, _ in server.requests))

    def test_queue_history_and_download(self):
        def respond(path, body):
            if path.startswith("/object_info/"):
                return 200, {"SaveImage": {"input": {"required": {}}}}
            if path == "/prompt":
                self.assertIn("client_id", json.loads(body))
                return 200, {"prompt_id": "job"}
            if path == "/history/job":
                return 200, {"job": {"outputs": {"7": {"images": [{"filename": "result image.png", "subfolder": "中文", "type": "output"}]}}}}
            return 200, b"test-image"
        server = Server(respond)
        self.addCleanup(server.close)
        graph = {"7": {"class_type": "SaveImage", "inputs": {}}}
        self.assertEqual(server.client.execute(graph, ["7"], progress=lambda _: None), [b"test-image"])
        self.assertTrue(any("filename=result+image.png" in path for _, path, _ in server.requests))
        self.assertEqual(sum(path == "/prompt" for _, path, _ in server.requests), 1)

    def test_failure_does_not_retry_or_interrupt(self):
        def respond(path, body):
            if path.startswith("/object_info/"):
                return 200, {"SaveImage": {"input": {"required": {}}}}
            if path == "/prompt":
                return 200, {"prompt_id": "job"}
            return 200, {"job": {"status": {"status_str": "error", "messages": ["out of memory"]}}}
        server = Server(respond)
        self.addCleanup(server.close)
        with self.assertRaisesRegex(ComfyError, "out of memory"):
            server.client.execute({"7": {"class_type": "SaveImage", "inputs": {}}}, ["7"], progress=lambda _: None)
        self.assertEqual(sum(path == "/prompt" for _, path, _ in server.requests), 1)
        self.assertFalse(any("interrupt" in path for _, path, _ in server.requests))

    def test_mask_preserves_outside_and_replaces_with_transparency(self):
        from PIL import Image
        original = Image.new("RGBA", (4, 4), (20, 30, 40, 87))
        generated = Image.new("RGBA", (4, 4), (100, 120, 140, 0))
        buffer = io.BytesIO()
        generated.save(buffer, format="PNG")
        mask = Image.new("L", (4, 4), 0)
        mask.putpixel((2, 2), 255)
        result = Image.open(io.BytesIO(local_composite(original, buffer.getvalue(), mask)))
        self.assertEqual(result.getpixel((0, 0)), original.getpixel((0, 0)))
        self.assertEqual(result.getpixel((2, 2)), generated.getpixel((2, 2)))
        with self.assertRaises(ComfyError):
            local_composite(original, buffer.getvalue(), Image.new("L", (2, 2)))


if __name__ == "__main__":
    unittest.main()
