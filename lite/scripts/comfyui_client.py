"""Local ComfyUI client: text generation, image edits, reference fusion and batches.

Only the Python standard library is needed for text-only tasks. Pillow is needed
for input images and exact local mask compositing. No cloud credentials are used.
"""
from __future__ import annotations

import argparse
import copy
import io
import json
import mimetypes
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


class ComfyError(RuntimeError):
    pass


class ComfyClient:
    def __init__(self, endpoint="http://127.0.0.1:8188", timeout=30):
        parsed = urllib.parse.urlsplit(endpoint)
        if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ComfyError("请填写 ComfyUI 服务基础地址，例如 http://127.0.0.1:8188")
        self.endpoint = endpoint.rstrip("/")
        self.timeout = timeout
        # Redirects can lead to another service; never silently submit images there.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *args):
                return None
        self.opener = urllib.request.build_opener(NoRedirect)

    def request(self, path, data=None, content_type="application/json", binary=False):
        body = json.dumps(data).encode("utf-8") if isinstance(data, dict) else data
        request = urllib.request.Request(self.endpoint + "/" + path.lstrip("/"), body,
                                         {"Content-Type": content_type})
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                raw = response.read(96 * 1024 * 1024 + 1)
                if len(raw) > 96 * 1024 * 1024:
                    raise ComfyError("服务响应超过 96 MiB 限制")
                if binary:
                    return raw
                result = json.loads(raw)
                if not isinstance(result, (dict, list)):
                    raise ComfyError("服务返回了无效数据")
                return result
        except urllib.error.HTTPError as error:
            # ComfyUI node errors contain useful model/node details, but no credentials.
            detail = error.read(8000).decode("utf-8", "replace")
            raise ComfyError(f"ComfyUI HTTP {error.code}：{detail[:2000]}") from None
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise ComfyError(f"无法连接 ComfyUI，请确认服务已启动、地址和端口正确：{error}") from None
        except (json.JSONDecodeError, UnicodeDecodeError):
            raise ComfyError("该地址没有返回 ComfyUI JSON，请检查是否填错端口或使用了网页地址") from None

    def check(self):
        stats = self.request("system_stats")
        if not isinstance(stats, dict) or not isinstance(stats.get("system"), dict):
            raise ComfyError("该地址没有返回 ComfyUI 服务信息")
        return stats

    def node_info(self, name):
        info = self.request("object_info/" + urllib.parse.quote(name))
        if name not in info:
            raise ComfyError(f"缺少节点 {name}，请更新 ComfyUI 或安装匹配的工作流节点")
        return info[name]

    def upload(self, image_bytes):
        name = "compositor-" + uuid.uuid4().hex + ".png"
        boundary = "Compositor" + uuid.uuid4().hex
        body = (f'--{boundary}\r\nContent-Disposition: form-data; name="image"; filename="{name}"\r\n'
                'Content-Type: image/png\r\n\r\n').encode() + image_bytes + (
                f'\r\n--{boundary}\r\nContent-Disposition: form-data; name="type"\r\n\r\ninput\r\n'
                f'--{boundary}--\r\n').encode()
        result = self.request("upload/image", body, "multipart/form-data; boundary=" + boundary)
        if not result.get("name"):
            raise ComfyError("图片上传未返回文件名称")
        return "/".join(part for part in (result.get("subfolder"), result["name"]) if part)

    def validate(self, graph):
        if not isinstance(graph, dict) or not graph or any(not isinstance(n, dict) or "class_type" not in n or not isinstance(n.get("inputs"), dict) for n in graph.values()):
            raise ComfyError("工作流须为 API 格式（节点包含 class_type 和 inputs），不能使用界面导出的 nodes/links 格式")
        # Validate node availability, required inputs and model choices before queueing.
        cache = {}
        for node in graph.values():
            kind, values = node["class_type"], node["inputs"]
            if kind not in cache:
                cache[kind] = self.node_info(kind)
            inputs = cache[kind].get("input", {})
            for name, spec in inputs.get("required", {}).items():
                if name not in values:
                    raise ComfyError(f"{kind} 缺少参数 {name}")
            for name, spec in {**inputs.get("required", {}), **inputs.get("optional", {})}.items():
                value = values.get(name)
                if value is not None and spec and isinstance(spec[0], list) and not isinstance(value, list) and value not in spec[0]:
                    raise ComfyError(f"{kind}.{name} 的值 {value!r} 不可用；请修改模型配置或工作流")

    def execute(self, graph, output_nodes, timeout=900, progress=print):
        self.validate(graph)
        started = self.request("prompt", {"prompt": graph, "client_id": "compositor-" + uuid.uuid4().hex})
        task = started.get("prompt_id")
        if not task:
            raise ComfyError("ComfyUI 未接受任务：" + json.dumps(started, ensure_ascii=False)[:1500])
        progress(f"任务已提交：{task}，正在等待本机处理…")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            history = self.request("history/" + urllib.parse.quote(task, safe="")).get(task, {})
            if history.get("status", {}).get("status_str") == "error":
                messages = history.get("status", {}).get("messages", [])
                raise ComfyError("ComfyUI 执行失败：" + json.dumps(messages, ensure_ascii=False)[:2000])
            outputs = history.get("outputs", {})
            files = [image for node in output_nodes for image in outputs.get(str(node), {}).get("images", [])]
            if files:
                return [self.request("view?" + urllib.parse.urlencode({k: info.get(k, "") for k in ("filename", "subfolder", "type")}), binary=True) for info in files]
            if history.get("status", {}).get("completed"):
                raise ComfyError("任务已结束但没有图片，请检查 output 映射是否指向 SaveImage 节点")
            time.sleep(0.5)
        # Never use /interrupt: that could stop somebody else's running task.
        raise ComfyError(f"等待任务 {task} 超时；服务可能仍在执行，没有自动重试或中断其他任务")


def node(kind, **inputs):
    return {"class_type": kind, "inputs": inputs}


def build_workflow(config, prompt, images, mask_name=None):
    family = config.get("family", "qwen-image-edit")
    width, height = int(config.get("width", 1024)), int(config.get("height", 1024))
    if not 64 <= width <= 8192 or not 64 <= height <= 8192 or width % 16 or height % 16:
        raise ComfyError("宽高须为 64–8192 之间的 16 的倍数")
    model = config.get("model", "")
    if not model:
        raise ComfyError("请在配置文件中填写 model 模型文件名")
    graph = {str(20 + i): node("LoadImage", image=name) for i, name in enumerate(images)}
    steps = int(config.get("steps", 20))
    if not 1 <= steps <= 200:
        raise ComfyError("steps 须为 1–200")
    if family == "qwen-image-edit":
        if len(images) > 3:
            raise ComfyError("千问图片编辑最多接收三张参考图；更多图片请加 --batch 分别处理")
        graph.update({
            "1": node("UNETLoader", unet_name=model, weight_dtype="default"),
            "2": node("CLIPLoader", clip_name=config["text_encoder"], type="qwen_image", device="default"),
            "3": node("VAELoader", vae_name=config["vae"]),
            "4": node("ModelSamplingAuraFlow", model=["1", 0], shift=config.get("shift", 3.1)),
            "5": node("CFGNorm", model=["4", 0], strength=1.0),
            "6": node("EmptySD3LatentImage", width=width, height=height, batch_size=1),
        })
        refs = {f"image{i + 1}": [str(20 + i), 0] for i in range(len(images))}
        graph["8"] = node("TextEncodeQwenImageEditPlus", clip=["2", 0], vae=["3", 0], prompt=prompt, **refs)
        graph["9"] = node("TextEncodeQwenImageEditPlus", clip=["2", 0], vae=["3", 0], prompt=config.get("negative_prompt", ""), **refs)
        model_link, positive, negative, latent, vae = ["5", 0], ["8", 0], ["9", 0], ["6", 0], ["3", 0]
    elif family == "checkpoint":
        if len(images) > 1:
            raise ComfyError("普通检查点模板每次使用一张图；多图融合请使用千问图片模板，自定义工作流或 --batch")
        graph["1"] = node("CheckpointLoaderSimple", ckpt_name=model)
        graph["8"] = node("CLIPTextEncode", clip=["1", 1], text=prompt)
        graph["9"] = node("CLIPTextEncode", clip=["1", 1], text=config.get("negative_prompt", ""))
        if images:
            graph["21"] = node("ImageScale", image=["20", 0], upscale_method="lanczos", width=width, height=height, crop="disabled")
            graph["6"] = node("VAEEncode", pixels=["21", 0], vae=["1", 2])
            if mask_name:
                graph["22"] = node("LoadImage", image=mask_name)
                graph["6"] = node("VAEEncodeForInpaint", pixels=["21", 0], vae=["1", 2], mask=["22", 1], grow_mask_by=0)
        else:
            graph["6"] = node("EmptyLatentImage", width=width, height=height, batch_size=1)
        model_link, positive, negative, latent, vae = ["1", 0], ["8", 0], ["9", 0], ["6", 0], ["1", 2]
    else:
        raise ComfyError("未知模型家族；不同架构请使用 --workflow API 工作流")
    graph["10"] = node("KSampler", model=model_link, positive=positive, negative=negative, latent_image=latent,
                       seed=int(config.get("seed", 1)), steps=steps, cfg=float(config.get("cfg", 4 if family == "qwen-image-edit" else 7)),
                       sampler_name=config.get("sampler", "euler"), scheduler=config.get("scheduler", "simple" if family == "qwen-image-edit" else "normal"),
                       denoise=float(config.get("denoise", 1 if family == "qwen-image-edit" or not images else 0.65)))
    graph["11"] = node("VAEDecode", samples=["10", 0], vae=vae)
    graph["7"] = node("SaveImage", images=["11", 0], filename_prefix="Compositor")
    return graph


def apply_bindings(graph, bindings, values):
    graph = copy.deepcopy(graph)
    for key, address in bindings.items():
        if key == "output":
            continue
        if key not in values:
            raise ComfyError(f"工作流需要输入 {key}，请补充图片或参数")
        target, separator, field = str(address).partition(".")
        if not separator or target not in graph or field not in graph[target].get("inputs", {}):
            raise ComfyError(f"无效映射 {key}: {address}")
        graph[target]["inputs"][field] = values[key]
    if "prompt" not in bindings or "output" not in bindings:
        raise ComfyError("自定义工作流必须提供 prompt 与 output 映射")
    return graph


def pillow():
    try:
        from PIL import Image, ImageOps
        return Image, ImageOps
    except ImportError:
        raise ComfyError("图片输入需要 Pillow。请先运行：python -m pip install Pillow") from None


def png_bytes(image):
    buffer = io.BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue()


def load_image(path):
    Image, ImageOps = pillow()
    with Image.open(path) as image:
        image.seek(0)
        return ImageOps.exif_transpose(image).convert("RGBA")


def local_composite(original, generated, mask):
    Image, _ = pillow()
    if mask.size != original.size:
        raise ComfyError("蒙版尺寸必须与第一张输入图一致，白色为修改区，黑色为保留区")
    with Image.open(io.BytesIO(generated)) as image:
        result = image.convert("RGBA").resize(original.size, Image.Resampling.LANCZOS)
    return png_bytes(Image.composite(result, original, mask.convert("L")))


def main(argv=None):
    parser = argparse.ArgumentParser(description="ComfyUI 中文修图脚本：文字生图、单图修改、多图参考、选区修改及批量处理")
    parser.add_argument("--config", type=Path, default=Path(__file__).resolve().parents[1] / "workflows/comfyui/settings.example.json")
    parser.add_argument("--url", help="覆盖本地服务地址")
    parser.add_argument("--prompt", help="中文想法；不填写则交互输入")
    parser.add_argument("--input", type=Path, nargs="*", default=[], help="一张或多张参考图片；顺序对应图1/图2/图3")
    parser.add_argument("--mask", type=Path, help="白色修改、黑色保留的蒙版，作用于图1")
    parser.add_argument("--batch", action="store_true", help="每张图片分别处理，不进行多图融合")
    parser.add_argument("--workflow", type=Path, help="自定义 API 工作流 JSON，映射放在配置的 bindings 中")
    parser.add_argument("--output", type=Path, default=Path("comfyui-output"))
    parser.add_argument("--check", action="store_true", help="仅检测连接、列出本机模型，不生成图片")
    parser.add_argument("--dry-run", type=Path, help="上传输入并验证节点，导出 API 工作流，不提交生图")
    args = parser.parse_args(argv)
    try:
        config = json.loads(args.config.read_text(encoding="utf-8-sig"))
        client = ComfyClient(args.url or config.get("endpoint", "http://127.0.0.1:8188"))
        stats = client.check()
        print("ComfyUI 已连接：", stats["system"].get("comfyui_version", "未知版本"))
        if args.check:
            for name, field in (("CheckpointLoaderSimple", "ckpt_name"), ("UNETLoader", "unet_name"), ("CLIPLoader", "clip_name"), ("VAELoader", "vae_name")):
                choices = client.node_info(name)["input"]["required"][field][0]
                print(name + ":", json.dumps(choices, ensure_ascii=False))
            return 0
        prompt = args.prompt if args.prompt is not None else input("请输入你的修图想法：").strip()
        if not prompt.strip() or len(prompt) > 8000:
            raise ComfyError("请输入 1–8000 字的修图想法")
        if args.mask and (not args.input or args.batch and len(args.input) > 1):
            raise ComfyError("局部蒙版需要一张基础图片；批量时请分别提供匹配蒙版")
        groups = [[path] for path in args.input] if args.batch and args.input else [args.input]
        args.output.mkdir(parents=True, exist_ok=True)
        for index, paths in enumerate(groups):
            originals = [load_image(path) for path in paths]
            names = [client.upload(png_bytes(image)) for image in originals]
            mask, mask_name = None, None
            if args.mask:
                Image, _ = pillow()
                mask = load_image(args.mask).convert("L")
                if mask.size != originals[0].size:
                    raise ComfyError("蒙版尺寸必须与图1一致；没有提交生图任务")
                resized = mask.resize((config.get("width", 1024), config.get("height", 1024)), Image.Resampling.LANCZOS)
                rgba = Image.new("RGBA", resized.size, "white")
                from PIL import ImageOps
                rgba.putalpha(ImageOps.invert(resized))  # ComfyUI LoadImage exposes inverse alpha as MASK.
                mask_name = client.upload(png_bytes(rgba))
            if args.workflow:
                graph = json.loads(args.workflow.read_text(encoding="utf-8-sig"))
                values = {**config, "prompt": prompt, **{f"image{i+1}": n for i, n in enumerate(names)}}
                if names:
                    values["image"] = names[0]
                if mask_name:
                    values["mask"] = mask_name
                graph = apply_bindings(graph, config.get("bindings", {}), values)
                output_nodes = config["bindings"]["output"]
                output_nodes = output_nodes if isinstance(output_nodes, list) else [output_nodes]
            else:
                graph = build_workflow(config, prompt, names, mask_name)
                output_nodes = ["7"]
            if args.dry_run:
                client.validate(graph)
                destination = args.dry_run if len(groups) == 1 else args.dry_run.with_stem(args.dry_run.stem + f"-{index+1}")
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(json.dumps(graph, ensure_ascii=False, indent=2), encoding="utf-8")
                print("工作流已验证，未生图：", destination)
                continue
            for number, data in enumerate(client.execute(graph, output_nodes, timeout=float(config.get("timeout", 900)))):
                if mask is not None:
                    data = local_composite(originals[0], data, mask)
                target = args.output / f"{time.time_ns()}-{index+1}-{number+1}.png"
                # A temporary file ensures an interrupted download cannot overwrite an earlier version.
                temp = target.with_suffix(".tmp")
                temp.write_bytes(data)
                temp.replace(target)
                print("已保存新版本（原文件保留）：", target)
        return 0
    except KeyboardInterrupt:
        print("已停止等待；没有中断其他用户的 ComfyUI 任务。")
        return 130
    except (ComfyError, OSError, ValueError, KeyError) as error:
        print("处理未完成：", error)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
