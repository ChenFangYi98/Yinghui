"""Build the offline Qwen pack from pinned, hash-verified upstream assets."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import json
import shutil
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / '.tools/embedded-ai'
RELEASE = 'b11560'
REVISION = 'f6d5376be1edb4d416d56da11e5397a961aca8ae'
RUNTIME = {
    'cpu': ('llama-b11560-bin-win-cpu-x64.zip', 'a8c9ad8c39957294dbb3d694251fe43f98f481bff118e579f133b4dbbd545099'),
    'vulkan': ('llama-b11560-bin-win-vulkan-x64.zip', '2ef19cf51dd5f037dcac25e57f2299b4ffae0c9620b83a14cf9b3f60860b72fc'),
    'cuda': ('llama-b11560-bin-win-cuda-12.4-x64.zip', '5b0e7d7d6a0c6908005459a3371463d1b125b036ba99fbe9cb2f5e1636d85725'),
    'cudart': ('cudart-llama-bin-win-cuda-12.4-x64.zip', '8c79a9b226de4b3cacfd1f83d24f962d0773be79f1e7b75c6af4ded7e32ae1d6'),
}
WEIGHTS = {
    'Qwen3.5-2B-Q4_K_M.gguf': 'aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223',
    'mmproj-F16.gguf': '7035e9cb8d7c6a9681d07eef9a364783e86ea4cd73faab2eabb4f43a101830c7',
}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fetch(url, path, sha=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and (sha is None or digest(path) == sha):
        return path
    temporary = path.with_suffix(path.suffix + '.part')
    print('Downloading', path.name, flush=True)
    request = urllib.request.Request(url, headers={'User-Agent': 'Compositor-offline-pack'})
    with urllib.request.urlopen(request, timeout=120) as response, temporary.open('wb') as output:
        shutil.copyfileobj(response, output, length=1024 * 1024)
    if sha and digest(temporary) != sha:
        raise RuntimeError('Checksum mismatch: ' + path.name)
    temporary.replace(path)
    print('Verified', path.name, flush=True)
    return path


def runtime(backend, asset):
    name, sha = asset
    path = fetch(f'https://github.com/ggml-org/llama.cpp/releases/download/{RELEASE}/{name}',
                 ROOT / '.tools/downloads' / name, sha)
    target = PACK / ('cuda' if backend == 'cudart' else backend)
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path) as archive:
        for item in archive.infolist():
            leaf = Path(item.filename).name
            if leaf == 'llama-server.exe' or leaf.lower().endswith('.dll'):
                with archive.open(item) as source, (target / leaf).open('wb') as output:
                    shutil.copyfileobj(source, output)
            elif leaf.startswith('LICENSE'):
                (PACK / leaf).write_bytes(archive.read(item))


def main():
    PACK.mkdir(parents=True, exist_ok=True)
    previous = json.loads((PACK / 'manifest.json').read_text(encoding='utf-8')) if (PACK / 'manifest.json').is_file() else {}
    with ThreadPoolExecutor(max_workers=4) as pool:
        jobs = [pool.submit(runtime, backend, asset) for backend, asset in RUNTIME.items()]
        jobs += [pool.submit(fetch,
            f'https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/{REVISION}/{name}?download=true&compositor=042',
            PACK / 'models' / name, sha) for name, sha in WEIGHTS.items()]
        for job in jobs:
            job.result()
    fetch(f'https://raw.githubusercontent.com/ggml-org/llama.cpp/{RELEASE}/LICENSE', PACK / 'llama.cpp-LICENSE.txt')
    fetch('https://huggingface.co/Qwen/Qwen3.5-2B/resolve/main/LICENSE', PACK / 'Qwen-LICENSE.txt')
    fetch('https://docs.nvidia.com/cuda/eula/index.html', PACK / 'NVIDIA-CUDA-EULA.html')
    files = {str(p.relative_to(PACK)).replace('\\', '/'): {'sha256': digest(p), 'bytes': p.stat().st_size}
             for p in sorted(PACK.rglob('*')) if p.is_file() and p.name != 'manifest.json' and not p.name.endswith('.part')}
    manifest = {'version': 1, 'model': 'Qwen3.5-2B-Q4_K_M', 'runtime': RELEASE,
                'source': 'https://huggingface.co/unsloth/Qwen3.5-2B-GGUF', 'revision': REVISION,
                'weights': 'models/Qwen3.5-2B-Q4_K_M.gguf', 'projector': 'models/mmproj-F16.gguf',
                'files': files}
    if previous.get('models'):
        manifest['models'] = previous['models']
    (PACK / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print('Offline pack ready:', PACK, flush=True)


if __name__ == '__main__':
    main()
