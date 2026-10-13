"""Create an offline SD 1.5 image pack; no Python is needed by the shipped app."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import shutil
import zipfile
import time
import ctypes
from bootstrap_embedded_ai import fetch, digest, ROOT

PACK = ROOT / '.tools/embedded-image'
TAG = 'master-956-1b0ba10'
REVISION = '031b5f5df991f511b3f5fa8fed6d99048ababb69'
MODEL = 'stable-diffusion-v1-5-pruned-emaonly-Q4_0.gguf'
SHA = 'b8944e9fe0b69b36ae1b5bb0185b3a7b8ef14347fe0fa9af6c64c4829022261f'
RUNTIME_SHA = {
    'cpu': '39c2ea1cc03f697864c858ad0b22b9b8ce62f945ba428e8596a814527e6db6fb',
    'vulkan': '9e8b9baff514fe5dfb77e51bf08d85f11e6f10128eac14d4474c87f563309725',
}


def utf8_manifest(path):
    # Windows 10 1903+ converts main() arguments to UTF-8 when declared here.
    # The upstream image CLI otherwise receives ACP bytes for Chinese paths.
    data = b'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
 <assemblyIdentity version="1.0.0.0" processorArchitecture="amd64" name="Compositor.LocalImageEngine" type="win32"/>
 <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security></trustInfo>
 <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings><activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage></windowsSettings></application>
</assembly>'''
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.BeginUpdateResourceW.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
    api.BeginUpdateResourceW.restype = ctypes.c_void_p
    api.UpdateResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ushort, ctypes.c_void_p, ctypes.c_uint]
    api.UpdateResourceW.restype = ctypes.c_int
    api.EndUpdateResourceW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    api.EndUpdateResourceW.restype = ctypes.c_int
    handle = api.BeginUpdateResourceW(str(path), False)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        buffer = ctypes.create_string_buffer(data)
        if not api.UpdateResourceW(handle, 24, 1, 0, buffer, len(data)):
            raise ctypes.WinError(ctypes.get_last_error())
    except BaseException:
        api.EndUpdateResourceW(handle, True)
        raise
    if not api.EndUpdateResourceW(handle, False):
        raise ctypes.WinError(ctypes.get_last_error())


def runtime(backend):
    name = f'sd-master-1b0ba10-bin-win-{backend}-x64.zip'
    archive = fetch(f'https://github.com/leejet/stable-diffusion.cpp/releases/download/{TAG}/{name}',
                    ROOT / '.tools/downloads' / name, RUNTIME_SHA[backend])
    target = PACK / backend
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            leaf = Path(info.filename).name
            if leaf == 'sd-cli.exe' or leaf.lower().endswith('.dll'):
                with z.open(info) as source, (target / leaf).open('wb') as output:
                    shutil.copyfileobj(source, output)
            elif leaf.endswith('.txt'):
                (PACK / (leaf + '.LICENSE.txt')).write_bytes(z.read(info))
    utf8_manifest(target / 'sd-cli.exe')


def main():
    with ThreadPoolExecutor(max_workers=3) as pool:
        jobs = [pool.submit(runtime, backend) for backend in ['cpu', 'vulkan']]
        jobs.append(pool.submit(fetch,
            f'https://huggingface.co/second-state/stable-diffusion-v1-5-GGUF/resolve/{REVISION}/{MODEL}?download=true&cache={time.time_ns()}',
            PACK / 'models' / MODEL, SHA))
        for job in jobs:
            job.result()
    fetch(f'https://raw.githubusercontent.com/leejet/stable-diffusion.cpp/{TAG}/LICENSE', PACK / 'stable-diffusion.cpp-LICENSE.txt')
    fetch('https://huggingface.co/spaces/CompVis/stable-diffusion-license/resolve/main/license.txt', PACK / 'SD1.5-LICENSE.txt')
    fetch('https://raw.githubusercontent.com/webmproject/libwebp/main/COPYING', PACK / 'libwebp-LICENSE.txt')
    fetch('https://raw.githubusercontent.com/webmproject/libwebm/main/LICENSE.TXT', PACK / 'libwebm-LICENSE.txt')
    files = {str(p.relative_to(PACK)).replace('\\', '/'): {'sha256': digest(p), 'bytes': p.stat().st_size}
             for p in sorted(PACK.rglob('*')) if p.is_file() and p.name != 'manifest.json'}
    manifest = {'version': 1, 'model': 'SD 1.5 Q4', 'runtime': TAG, 'source': 'https://huggingface.co/second-state/stable-diffusion-v1-5-GGUF',
                'revision': REVISION, 'weights': 'models/' + MODEL,
                'launcherModification': 'Windows UTF-8 activeCodePage manifest', 'files': files}
    (PACK / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print('Offline image pack ready:', PACK, flush=True)


if __name__ == '__main__':
    main()
