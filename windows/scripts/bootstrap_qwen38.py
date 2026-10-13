"""Install pinned Qwen3.8 vision weights beside the small offline assistant.

Downloads resume after interruption; only verified files enter the model catalog.
No global Python, CUDA or user ComfyUI environment is changed.
"""
from pathlib import Path
import hashlib
import json
import time
import urllib.request

from bootstrap_embedded_ai import PACK, digest

REVISION = '4ca720788d1e01f1bff70c033e0d0028fd02e502'
ASSETS = {
    'Qwen3.8-27B-UD-Q4_K_M.gguf': (16464440224, '322e194ff79741c7baa497c240f677f54b201b0efab44ca8e50f122b39123482'),
    'mmproj-F16.gguf': (927607488, 'cbb841a9ee0636b2ec172f5bb8df2ea8dfeb01e90fe7c6126581d662a0b4e43e'),
}


def download(name, size, sha):
    path = PACK / 'models/qwen38' / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.stat().st_size == size and digest(path) == sha:
        return path
    partial = path.with_suffix(path.suffix + '.part')
    for attempt in range(12):
        offset = partial.stat().st_size if partial.exists() else 0
        if offset == size:
            break
        if offset > size:
            raise RuntimeError('Partial download is too large: ' + name)
        url = f'https://huggingface.co/unsloth/Qwen3.8-27B-GGUF/resolve/{REVISION}/{name}?download=true&yinghui={int(time.time())}'
        request = urllib.request.Request(url, headers={'Range': f'bytes={offset}-', 'User-Agent': 'Yinghui-local-models'})
        try:
            with urllib.request.urlopen(request, timeout=90) as response:
                if offset and (response.status != 206 or not response.headers.get('Content-Range', '').startswith(f'bytes {offset}-')):
                    raise RuntimeError('Server did not honor resume range')
                with partial.open('ab') as stream:
                    started = last = time.monotonic()
                    while block := response.read(8 * 1024 * 1024):
                        stream.write(block)
                        offset += len(block)
                        if time.monotonic() - last > 10:
                            print(f'{name}: {offset / size:.1%} ({offset / 1e9:.2f} GB)', flush=True)
                            last = time.monotonic()
            if partial.stat().st_size == size:
                break
        except Exception as error:
            print(f'Resumable download attempt {attempt + 1}: {type(error).__name__}', flush=True)
            time.sleep(min(2 * (attempt + 1), 20))
    if not partial.exists() or partial.stat().st_size != size or digest(partial) != sha:
        raise RuntimeError('Model checksum or size mismatch: ' + name)
    partial.replace(path)
    print('Verified', name, flush=True)
    return path


def main():
    manifest_path = PACK / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    for name, (size, sha) in ASSETS.items():
        path = download(name, size, sha)
        manifest['files'][path.relative_to(PACK).as_posix()] = {'bytes': size, 'sha256': sha}
    license_path = PACK / 'Qwen3.8-LICENSE.txt'
    license_path.write_bytes(urllib.request.urlopen(
        'https://huggingface.co/Qwen/Qwen3.8-27B/resolve/1d4bf0f2ff6012fd82039f2fa52739d0dd7c60c0/LICENSE', timeout=60).read())
    manifest['files'][license_path.name] = {'bytes': license_path.stat().st_size, 'sha256': digest(license_path)}
    # Preserve the original pack fields so older applications can still use 2B.
    manifest['models'] = [
        {'id': 'qwen35-2b', 'name': 'Qwen3.5 2B', 'weights': manifest['weights'], 'projector': manifest['projector']},
        {'id': 'qwen38-27b', 'name': 'Qwen3.8 27B',
         'weights': 'models/qwen38/Qwen3.8-27B-UD-Q4_K_M.gguf',
         'projector': 'models/qwen38/mmproj-F16.gguf', 'revision': REVISION,
         'source': 'https://huggingface.co/unsloth/Qwen3.8-27B-GGUF'},
    ]
    temporary = manifest_path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    for attempt in range(300):
        try:
            temporary.replace(manifest_path)
            break
        except PermissionError:
            if attempt == 299:
                raise RuntimeError('Close the model verifier and rerun; verified weights have been retained')
            time.sleep(1)
    print('Qwen3.8 offline catalog ready', flush=True)


if __name__ == '__main__':
    main()
