"""Isolated local Qwen image engine. The application never imports this environment.

The developer copy may be edited explicitly; the shipped copy remains recoverable.
All model loading is offline after a verified installation, without remote code.
"""
import argparse
import ctypes
import gc
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import io
import asyncio
import uuid

TORCH_WHEELS = {
    'cu128': [
        'https://download.pytorch.org/whl/cu128/torch-2.7.1%2Bcu128-cp312-cp312-win_amd64.whl#sha256=2bb8c05d48ba815b316879a18195d53a6472a03e297d971e916753f8e1053d30',
        'https://download.pytorch.org/whl/cu128/torchvision-0.22.1%2Bcu128-cp312-cp312-win_amd64.whl#sha256=650561ba326d21021243f5e064133dc62dc64d52f79623db5cd76637a9665f96',
    ],
    'cpu': [
        'https://download.pytorch.org/whl/cpu/torch-2.7.1%2Bcpu-cp312-cp312-win_amd64.whl#sha256=0bc887068772233f532b51a3e8c8cfc682ae62bef74bf4e0c53526c8b9e4138f',
        'https://download.pytorch.org/whl/cpu/torchvision-0.22.1%2Bcpu-cp312-cp312-win_amd64.whl#sha256=433cb4dbced7291f17064cea08ac1e5aebd02ec190e1c207d117ad62a8961f2b',
    ],
}


def event(kind, **values):
    print(json.dumps({'event': kind, **values}, ensure_ascii=False), flush=True)


def atomic_json(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
    temporary.replace(path)


def model_spec(catalog, identifier):
    data = json.loads(Path(catalog).read_text(encoding='utf-8'))
    if data.get('version') != 1:
        raise ValueError('Invalid model catalog version')
    for model in data['models']:
        if model['id'] == identifier:
            for item in model['files']:
                path = Path(item['path'])
                if path.is_absolute() or '..' in path.parts or '\\' in item['path'] or ':' in item['path']:
                    raise ValueError('Invalid model asset path')
                if len(item['sha256']) != 64 or item['bytes'] <= 0:
                    raise ValueError('Invalid model asset checksum')
            return model
    raise ValueError('Unknown local model')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def configure(root, offline=False):
    os.environ['HF_HOME'] = str(root / 'cache')
    os.environ['HF_HUB_DISABLE_TELEMETRY'] = '1'
    os.environ['DO_NOT_TRACK'] = '1'
    os.environ['PYTHONNOUSERSITE'] = '1'
    os.environ['TOKENIZERS_PARALLELISM'] = 'false'
    # Public, pinned models never inherit an account token or a cloud endpoint.
    for name in ['HF_TOKEN', 'HUGGING_FACE_HUB_TOKEN', 'HF_ENDPOINT', 'PYTHONPATH', 'PYTHONHOME']:
        os.environ.pop(name, None)
    if offline:
        os.environ['HF_HUB_OFFLINE'] = '1'
        os.environ['TRANSFORMERS_OFFLINE'] = '1'
    else:
        os.environ.pop('HF_HUB_OFFLINE', None)
        os.environ.pop('TRANSFORMERS_OFFLINE', None)


def cuda_driver():
    try:
        cuda = ctypes.WinDLL('nvcuda')
        return cuda.cuInit(0) == 0
    except (OSError, AttributeError):
        return False


def pip_install(arguments):
    event('progress', text='Installing isolated inference dependencies')
    process = subprocess.Popen([sys.executable, '-m', 'pip', 'install', '--disable-pip-version-check',
                                '--only-binary=:all:', '--timeout', '30', '--retries', '2', *arguments],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding='utf-8', errors='replace')
    last = ''
    for line in process.stdout:
        last = line.strip()
        if last.startswith(('Collecting ', 'Downloading ', 'Using cached ', 'Installing collected packages:', 'Successfully installed')):
            event('progress', text=last[:300])
    if process.wait():
        raise RuntimeError('Dependency installation failed: ' + last[:300])


def environment_probe():
    import torch
    import diffusers
    import transformers
    from diffusers import QwenImage21Pipeline, QwenImageEditPlusPipeline
    available = torch.cuda.is_available()
    return {'python': sys.version.split()[0], 'torch': torch.__version__, 'diffusers': diffusers.__version__,
            'transformers': transformers.__version__, 'cuda': available,
            'device': torch.cuda.get_device_name(0) if available else 'CPU'}


def install_environment(root, backend):
    ready = root / 'environment.json'
    if ready.exists():
        try:
            result = environment_probe()
            event('environment', **result)
            return
        except Exception:
            ready.unlink(missing_ok=True)
    use_cuda = backend != 'cpu' and cuda_driver()
    channel = 'cu128' if use_cuda else 'cpu'
    try:
        pip_install(TORCH_WHEELS[channel])
    except RuntimeError:
        if not use_cuda:
            raise
        event('progress', text='CUDA installation unavailable; installing CPU runtime')
        pip_install(['--force-reinstall', *TORCH_WHEELS['cpu']])
    pip_install(['diffusers==0.41.0', 'transformers==5.19.0', 'accelerate==1.15.0',
                 'safetensors==0.8.0', 'pillow==12.3.0', 'sentencepiece==0.2.1'])
    result = environment_probe()
    atomic_json(ready, result)
    event('environment', **result)


def verify_model(root, spec, full=False):
    folder = root / 'models' / spec['id']
    marker = folder / 'verified.json'
    previous = json.loads(marker.read_text(encoding='utf-8')) if marker.exists() else {}
    previous_files = previous.get('files', {}) if previous.get('revision') == spec['revision'] else {}
    verified = {}
    for item in spec['files']:
        path = folder / item['path']
        if not path.is_file() or path.stat().st_size != item['bytes']:
            raise RuntimeError('Missing or incomplete model asset: ' + item['path'])
        stat = path.stat()
        stamp = {'bytes': stat.st_size, 'mtime': stat.st_mtime_ns, 'sha256': item['sha256']}
        if full or previous_files.get(item['path']) != stamp:
            event('progress', text='Verifying ' + item['path'])
            if digest(path) != item['sha256']:
                raise RuntimeError('Model checksum mismatch: ' + item['path'])
        verified[item['path']] = stamp
    atomic_json(marker, {'revision': spec['revision'], 'files': verified})
    return folder


def download_model(root, spec, accept_research):
    if spec['license'] == 'research' and not accept_research:
        raise ValueError('Qwen-Image-2.1 requires acceptance of its research/evaluation license')
    from huggingface_hub import hf_hub_download
    folder = root / 'models' / spec['id']
    folder.mkdir(parents=True, exist_ok=True)
    missing = sum(item['bytes'] for item in spec['files']
                  if not (folder / item['path']).exists() or (folder / item['path']).stat().st_size != item['bytes'])
    if shutil.disk_usage(root).free < missing + 5 * 1024**3:
        raise RuntimeError('Insufficient disk space; keep at least model size plus 5 GiB free')
    for index, item in enumerate(spec['files']):
        event('progress', text='Downloading ' + item['path'], file=index + 1, total=len(spec['files']))
        path = folder / item['path']
        if path.exists() and path.stat().st_size == item['bytes'] and digest(path) == item['sha256']:
            continue
        # A corrupt complete file must be fetched again, rather than trusted by a cache marker.
        hf_hub_download(repo_id=spec['repo'], filename=item['path'], revision=spec['revision'],
                        local_dir=folder, token=False, force_download=path.exists())
    verify_model(root, spec, full=True)
    if spec['license'] == 'research':
        atomic_json(folder / 'license-acceptance.json', {'license': 'Qwen Research License',
                    'acceptedAt': time.strftime('%Y-%m-%dT%H:%M:%S'), 'use': 'research/evaluation'})
    event('installed', model=spec['id'], revision=spec['revision'])


def request_options(data, spec):
    operation = data.get('operation', 'generate')
    if operation not in ['generate', 'edit', 'inpaint', 'background', 'outpaint']:
        raise ValueError('Unsupported image operation')
    if operation == 'generate' and not spec['generate']:
        raise ValueError('This model edits existing images; select Qwen-Image-2.1 for generation')
    prompt = data.get('prompt', '').strip()
    if not prompt or len(prompt) > 8000:
        raise ValueError('Image prompt must contain 1 to 8000 characters')
    edge = data.get('maxEdge', 1024)
    steps = data.get('steps', 40)
    device = data.get('device', 'auto')
    if edge not in [256, 512, 768, 1024, 1536, 2048] or not isinstance(steps, int) or not 1 <= steps <= 80:
        raise ValueError('Invalid resolution or inference steps')
    if device not in ['auto', 'cpu', 'cuda']:
        raise ValueError('Invalid inference device')
    width, height = data.get('width', 1024), data.get('height', 1024)
    if not isinstance(width, int) or not isinstance(height, int) or not 1 <= width <= 30000 or not 1 <= height <= 30000:
        raise ValueError('Invalid image dimensions')
    scale = min(1.0, edge / max(width, height))
    size = tuple(max(64, round(n * scale / 16) * 16) for n in [width, height])
    return operation, prompt, size, steps, device


def infer(root, spec, data, output):
    operation, prompt, size, steps, preference = request_options(data, spec)
    folder = verify_model(root, spec)
    if spec['license'] == 'research' and not (folder / 'license-acceptance.json').exists():
        raise ValueError('Research license has not been accepted')
    import torch
    from PIL import Image
    from diffusers import QwenImage21Pipeline, QwenImageEditPlusPipeline
    torch.set_num_threads(min(8, max(1, (os.cpu_count() or 4) - 2)))
    cuda = preference != 'cpu' and torch.cuda.is_available()
    if preference == 'cuda' and not cuda:
        event('progress', text='CUDA unavailable; using CPU')
    cls = QwenImage21Pipeline if spec['id'] == 'qwen-image-21' else QwenImageEditPlusPipeline
    if sys.platform == 'win32':
        class MemoryStatus(ctypes.Structure):
            _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong),
                        *[(name, ctypes.c_ulonglong) for name in ['total', 'available', 'page', 'free_page', 'virtual', 'free_virtual', 'extended']]]
        memory = MemoryStatus()
        memory.length = ctypes.sizeof(memory)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory))
        needed = sum(item['bytes'] for item in spec['files']) * (1.05 if cuda else 2.1) + 4 * 1024**3
        if memory.available < needed:
            raise RuntimeError('Insufficient available RAM for this large model; use the built-in fallback or free memory')
    event('progress', text='Loading verified local model; no cloud request')
    pipe = cls.from_pretrained(folder, torch_dtype=torch.bfloat16 if cuda else torch.float32,
                              local_files_only=True, use_safetensors=True, trust_remote_code=False)
    if cuda:
        if torch.cuda.get_device_properties(0).total_memory < 24 * 1024**3:
            pipe.enable_sequential_cpu_offload()
        else:
            pipe.enable_model_cpu_offload()
    else:
        pipe.to('cpu')
    pipe.vae.enable_tiling()
    inputs = []
    if operation != 'generate':
        image = Image.open(data['image'])
        if image.width <= 0 or image.height <= 0:
            raise ValueError('Invalid source image')
        if data.get('mask'):
            mask = Image.open(data['mask'])
            if mask.size != image.size:
                raise ValueError('Image and selection mask dimensions differ')
            # Keep exact mask for compositing in C++; give the model a marked reference.
            overlay = image.convert('RGBA')
            tint = Image.new('RGBA', image.size, (255, 40, 80, 0))
            tint.putalpha(mask.convert('L').point(lambda value: round(value * 0.35)))
            overlay = Image.alpha_composite(overlay, tint).convert('RGB')
            inputs = [image.convert('RGB'), overlay]
            prompt += '\nEdit only the red-highlighted area in the second reference. Return the edited first image without marks; preserve all other content.'
        else:
            inputs = [image.convert('RGB')]
    def step_callback(pipeline, step, timestep, values):
        event('progress', text='Generating', step=step + 1, total=steps)
        return values
    kwargs = dict(prompt=prompt, width=size[0], height=size[1], num_inference_steps=steps,
                  generator=torch.Generator('cpu').manual_seed(int(data.get('seed', 1))),
                  callback_on_step_end=step_callback, true_cfg_scale=1.0 if spec['generate'] else 4.0)
    if inputs:
        kwargs['image'] = inputs
    if not spec['generate']:
        kwargs['negative_prompt'] = ''
    event('progress', text='Running on CUDA with CPU offload' if cuda else 'Running on CPU; large models can be slow')
    with torch.inference_mode():
        result = pipe(**kwargs).images[0]
    if operation != 'generate':
        result = result.resize(Image.open(data['image']).size, Image.Resampling.LANCZOS)
    result.save(output, format='PNG')
    event('result', output=str(output), width=result.width, height=result.height,
          device='cuda-offload' if cuda else 'cpu', model=spec['name'])
    del pipe
    gc.collect()
    if cuda:
        torch.cuda.empty_cache()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--model', default='qwen-image-21')
    parser.add_argument('--backend', choices=['auto', 'cpu'], default='auto')
    parser.add_argument('--accept-research-license', action='store_true')
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--install-environment', action='store_true')
    group.add_argument('--install-model', action='store_true')
    group.add_argument('--probe', action='store_true')
    group.add_argument('--request', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--comfy-endpoint')
    parser.add_argument('--comfy-pack', type=Path)
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    configure(args.root, offline=bool(args.request or args.probe))
    if args.comfy_endpoint:
        asyncio.run(run_comfy(args))
        return
    spec = model_spec(args.catalog, args.model)
    if args.install_environment:
        install_environment(args.root, args.backend)
    elif args.install_model:
        download_model(args.root, spec, args.accept_research_license)
    elif args.probe:
        environment = environment_probe()
        verify_model(args.root, spec)
        event('ready', **environment, model=spec['name'])
    else:
        if not args.output:
            raise ValueError('An output file is required')
        data = json.loads(args.request.read_text(encoding='utf-8'))
        infer(args.root, spec, data, args.output)


def comfy_graph(manifest, prompt, images, size, steps, seed):
    def node(kind, **inputs): return {'class_type': kind, 'inputs': inputs}
    encode = dict(clip=['2', 0], vae=['3', 0], prompt=prompt, negative_prompt='', resolution=0 if images else max(size))
    graph = {
        '1': node('UNETLoader', unet_name=manifest['modelFile'], weight_dtype='default'),
        '2': node('CLIPLoader', clip_name=manifest['encoderFile'], type='qwen_image', device='default'),
        '3': node('VAELoader', vae_name=manifest['vaeFile']),
        '4': node('QwenImage21Cache', model=['1', 0], device='auto', dtype='default'),
    }
    for index, image in enumerate(images, 1):
        key = str(20 + index)
        graph[key] = node('LoadImage', image=image)
        encode['images.image_' + str(index)] = [key, 0]
    graph['5'] = node('TextEncodeQwenImage21', **encode)
    # The generic latent resize node assumes /8 latents; Qwen 2.1 uses /16.
    # Halving its pixel arguments preserves the requested final image dimensions.
    graph['6'] = node('LatentUpscale', samples=['5', 2], upscale_method='nearest-exact', width=size[0] // 2, height=size[1] // 2, crop='disabled')
    graph['7'] = node('KSampler', model=['4', 0], positive=['5', 0], negative=['5', 1], latent_image=['6', 0],
                      seed=seed, steps=steps, cfg=1.0, sampler_name='euler', scheduler='simple', denoise=1.0)
    graph['8'] = node('VAEDecode', samples=['7', 0], vae=['3', 0])
    graph['9'] = node('SaveImage', images=['8', 0], filename_prefix='Yinghui_' + uuid.uuid4().hex)
    return graph


def cleanup_comfy(root, files):
    for category, info in files:
        base = Path(root) / 'comfy-jobs' / category
        name = info.get('filename', info.get('name', ''))
        if not name.startswith('Yinghui_') or not name.endswith('.png') or Path(name).name != name:
            continue
        path = (base / info.get('subfolder', '') / name).resolve()
        if path.is_relative_to(base.resolve()):
            path.unlink(missing_ok=True)


async def run_comfy(args):
    files = []
    try:
        await _run_comfy(args, files)
    finally:
        cleanup_comfy(args.root, files)


async def _run_comfy(args, files):
    import aiohttp
    from PIL import Image
    token = os.environ.pop('YINGHUI_LOCAL_TOKEN', '')
    if not args.comfy_endpoint.startswith('http://127.0.0.1:') or not token:
        raise ValueError('A private loopback engine and temporary token are required')
    manifest = json.loads((args.comfy_pack / 'manifest.json').read_text(encoding='utf-8'))
    async with aiohttp.ClientSession(headers={'X-Yinghui-Token': token}, timeout=aiohttp.ClientTimeout(total=None, sock_connect=10, sock_read=60)) as client:
        async def get(path):
            async with client.get(args.comfy_endpoint + path) as response:
                response.raise_for_status()
                return await response.json()
        if args.probe:
            info = await get('/object_info/TextEncodeQwenImage21')
            if not info: raise RuntimeError('Engine does not support Qwen Image 2.1')
            event('ready', engine='private-comfy', model='Qwen-Image-2.1 INT8')
            return
        data = json.loads(args.request.read_text(encoding='utf-8'))
        operation, prompt, size, steps, device = request_options(data, {'generate': True})
        size = tuple(max(128, round(n / 32) * 32) for n in size)
        images = []
        async def upload(image):
            buffer = io.BytesIO(); image.save(buffer, format='PNG')
            form = aiohttp.FormData()
            form.add_field('image', buffer.getvalue(), filename='Yinghui_' + uuid.uuid4().hex + '.png', content_type='image/png')
            async with client.post(args.comfy_endpoint + '/upload/image', data=form) as response:
                response.raise_for_status(); result = await response.json()
                files.append(('input', result))
                return result['name']
        original = None
        if operation != 'generate':
            original = Image.open(data['image']).copy()
            source = original.resize(size, Image.Resampling.LANCZOS)
            images.append(await upload(source))
            if data.get('mask'):
                mask = Image.open(data['mask'])
                if mask.size != original.size: raise ValueError('Selection mask size differs from image')
                tint = Image.new('RGBA', size, (255, 40, 80, 0))
                tint.putalpha(mask.resize(size, Image.Resampling.LANCZOS).convert('L').point(lambda v: round(v * .35)))
                marked = Image.alpha_composite(source.convert('RGBA'), tint).convert('RGB')
                images.append(await upload(marked))
                prompt += '\n仅修改第二张参考图的红色标记区域，输出修改后的第一张图片，不要保留标记；其他区域保持原样。'
        graph = comfy_graph(manifest, prompt, images, size, steps, int(data.get('seed', 1)))
        identifier = uuid.uuid4().hex
        async with client.ws_connect(args.comfy_endpoint + '/ws?clientId=' + identifier, heartbeat=20) as ws:
            async with client.post(args.comfy_endpoint + '/prompt', json={'prompt': graph, 'client_id': identifier}) as response:
                result = await response.json()
                if response.status != 200 or not result.get('prompt_id'):
                    raise RuntimeError('Workflow validation failed: ' + json.dumps(result, ensure_ascii=False)[:1000])
                prompt_id = result['prompt_id']
            history = {}
            while not history:
                try:
                    message = await ws.receive(timeout=1)
                    if message.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                        raise RuntimeError('The private image engine disconnected')
                    if message.type == aiohttp.WSMsgType.TEXT:
                        item = json.loads(message.data)
                        detail = item.get('data', {})
                        if item.get('type') == 'progress':
                            event('progress', text='Generating', step=detail['value'], total=detail['max'])
                        elif item.get('type') == 'executing' and detail.get('node'):
                            event('progress', text='Executing image tool ' + str(detail['node']))
                        elif item.get('type') in ['execution_error', 'execution_interrupted'] and detail.get('prompt_id') == prompt_id:
                            raise RuntimeError(detail.get('exception_message', 'Canceled image inference'))
                except asyncio.TimeoutError:
                    pass
                history = (await get('/history/' + prompt_id)).get(prompt_id, {})
            outputs = history.get('outputs', {}).get('9', {}).get('images', [])
            if not outputs: raise RuntimeError('The local engine returned no image')
            from urllib.parse import urlencode
            image_info = outputs[0]
            files.append(('output', image_info))
            async with client.get(args.comfy_endpoint + '/view?' + urlencode(image_info)) as response:
                response.raise_for_status()
                content = bytearray()
                async for chunk in response.content.iter_chunked(1024 * 1024):
                    content.extend(chunk)
                    if len(content) > 64 * 1024 * 1024:
                        raise RuntimeError('Image response exceeds the local size limit')
            result = Image.open(io.BytesIO(content)).copy()
            if original: result = result.resize(original.size, Image.Resampling.LANCZOS)
            result.save(args.output, format='PNG')
            event('result', engine='private-comfy', model='Qwen-Image-2.1 INT8', width=result.width, height=result.height)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        event('error', type=type(error).__name__, text=str(error)[:1200])
        sys.exit(1)
