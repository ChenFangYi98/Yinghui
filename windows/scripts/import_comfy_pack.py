"""Copy a private, offline Comfy core/runtime/model pack; never copy user jobs or credentials."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

CORE_DIRS = ['comfy', 'comfy_api', 'comfy_api_nodes', 'comfy_config', 'comfy_execution', 'comfy_extras',
             'utils', 'app', 'api_server', 'blueprints', 'middleware', 'alembic_db']
CORE_FILES = ['main.py', 'server.py', 'nodes.py', 'node_helpers.py', 'folder_paths.py', 'execution.py',
              'cuda_malloc.py', 'latent_preview.py', 'comfyui_version.py', 'requirements.txt',
              'alembic.ini', 'LICENSE', 'protocol.py', 'hook_breaker_ac10a0.py']
MODEL_FILES = ['diffusion_models/qwen_image_2.1_int8_convrot.safetensors',
               'text_encoders/qwen3vl_8b_int8_convrot.safetensors', 'vae/qwen_image_2.1_vae_bf16.safetensors']


def copy(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.git', '.cache', '*.log'))
    elif source.is_file():
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or destination.stat().st_size != source.stat().st_size:
            shutil.copy2(source, destination)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True, help='Comfy Desktop instance containing ComfyUI and standalone-env')
    parser.add_argument('--models', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    core = args.source / 'ComfyUI'
    base = args.source / 'standalone-env'
    packages = core / '.venv/Lib/site-packages'
    if not (core / 'comfy_extras/nodes_qwen.py').exists() or 'TextEncodeQwenImage21' not in (core / 'comfy_extras/nodes_qwen.py').read_text(encoding='utf-8'):
        raise RuntimeError('This Comfy core does not support Qwen Image 2.1')
    for name in MODEL_FILES:
        if not (args.models / name).is_file():
            raise RuntimeError('Missing model: ' + name)
    args.output.mkdir(parents=True, exist_ok=True)
    for name in CORE_DIRS + CORE_FILES:
        copy(core / name, args.output / 'comfy' / name)
    print('Copied Comfy core without custom nodes, inputs, outputs, user settings or API credentials', flush=True)
    for path in base.iterdir():
        if path.name in ['DLLs', 'Lib', 'include', 'libs', 'LICENSE.txt'] or path.suffix == '.dll' or path.name in ['python.exe', 'pythonw.exe']:
            if path.name == 'Lib':
                for child in path.iterdir():
                    if child.name != 'site-packages': copy(child, args.output / 'runtime/Lib' / child.name)
            else: copy(path, args.output / 'runtime' / path.name)
    copy(packages, args.output / 'runtime/Lib/site-packages')
    print('Copied private Python environment', flush=True)
    for name in MODEL_FILES:
        copy(args.models / name, args.output / 'models' / name)
        print('Copied ' + name, flush=True)
    files = {}
    for path in sorted(args.output.rglob('*')):
        if path.is_file() and path.name != 'manifest.json' and '__pycache__' not in path.parts and path.suffix != '.pyc':
            with path.open('rb') as stream: sha = hashlib.file_digest(stream, 'sha256').hexdigest()
            files[path.relative_to(args.output).as_posix()] = {'sha256': sha, 'bytes': path.stat().st_size}
            if path.is_relative_to(args.output / 'models'):
                print('Verified ' + path.name, flush=True)
    (args.output / 'manifest.json').write_text(json.dumps({'version': 1, 'model': 'qwen-image-21',
        'license': 'Qwen Research License; research/evaluation only',
        'source': 'Local ComfyUI model files; Comfy-Org/Qwen-Image-2.1', 'files': files,
        'modelFile': MODEL_FILES[0].split('/')[-1], 'encoderFile': MODEL_FILES[1].split('/')[-1],
        'vaeFile': MODEL_FILES[2].split('/')[-1]}, indent=2), encoding='utf-8')
    print('Offline pack complete: ' + str(sum(v['bytes'] for v in files.values())) + ' bytes', flush=True)


if __name__ == '__main__': main()
