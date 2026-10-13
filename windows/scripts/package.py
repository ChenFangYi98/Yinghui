"""Deploy Qt/MSVC, verify the isolated app, and build ZIP and Inno installers."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'out/release'
DIST = ROOT / 'dist'
LITE = os.environ.get('YINGHUI_PACKAGE_LITE') == '1'
NAME = 'Yinghui-Windows-0.4.5-x64' + ('-Lite' if LITE else '')
STAGE = DIST / NAME
INSTALLER = DIST / (NAME + '-Installer')

def stage_copy(source, destination):
    source, destination = Path(source), Path(destination)
    if source.suffix in ('.gguf', '.onnx', '.safetensors'):
        if destination.exists():
            if os.path.samefile(source, destination): return str(destination)
            destination.unlink()
        try:
            os.link(source, destination)
            return str(destination)
        except OSError:
            pass
    return shutil.copy2(source, destination)

def fetch(url, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=120) as response:
        destination.write_bytes(response.read())

def installer_compiler():
    candidates = [ROOT / '.tools/Inno/ISCC.exe',
                  Path(os.environ.get('ProgramFiles(x86)', '')) / 'Inno Setup 6/ISCC.exe',
                  Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs/Inno Setup 6/ISCC.exe']
    for path in candidates:
        if path.is_file():
            return path
    url = 'https://api.github.com/repos/jrsoftware/issrc/releases/tags/is-6_7_3'
    request = urllib.request.Request(url, headers={'User-Agent': 'Compositor-build'})
    try:
        release = json.load(urllib.request.urlopen(request, timeout=60))
        asset = next(a for a in release['assets'] if a['name'] == 'innosetup-6.7.3.exe')
    except urllib.error.HTTPError:
        asset = {'browser_download_url':'https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe'}
    setup = ROOT / '.tools/downloads/innosetup-6.7.3.exe'
    if not setup.is_file():
        fetch(asset['browser_download_url'], setup)
    if asset.get('digest'):
        assert asset['digest'] == 'sha256:' + hashlib.sha256(setup.read_bytes()).hexdigest()
    # Verify the publisher before starting this workspace-local build dependency.
    script = "$s=Get-AuthenticodeSignature -LiteralPath $args[0]; if($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'Pyrsys'){exit 1}"
    verify=ROOT / '.tools/verify-installer.ps1'
    verify.write_text(script,encoding='utf-8')
    subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(verify), str(setup)], check=True)
    subprocess.run([str(setup), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER',
                    '/NOICONS', '/SP-', '/DIR=' + str(ROOT / '.tools/Inno')],
                   check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    return candidates[0]

def main():
    qt = Path(os.environ.get('COMPOSITOR_QT_ROOT', ROOT / '.tools/Qt'))
    cmake = shutil.which('cmake')
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installations = json.loads(subprocess.check_output([str(vswhere), '-products', '*', '-requires',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-format', 'json', '-utf8'], encoding='utf-8'))
    vs = Path(next((i for i in installations if i['installationVersion'].startswith('17.')), installations[0])['installationPath'])
    cmake = cmake or str(vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
    STAGE.mkdir(parents=True, exist_ok=True)
    subprocess.run([cmake, '--install', str(BUILD), '--prefix', str(STAGE), '--component', 'Runtime'], check=True)
    # Install the application and notices without installing dependency developer SDKs.
    (STAGE / 'Compositor.exe').unlink(missing_ok=True)
    shutil.copy2(BUILD / 'Compositor.exe', STAGE / 'Yinghui.exe')
    shutil.copy2(ROOT / 'assets/yinghui.ico', STAGE / 'Yinghui.ico')
    shutil.copytree(BUILD / 'fonts', STAGE / 'fonts', dirs_exist_ok=True)
    runtime = ROOT / '.tools/onnxruntime/onnxruntime-win-x64-1.20.1'
    if (runtime / 'lib/onnxruntime.dll').is_file():
        pack = ROOT / '.tools/vision-models'
        report = json.loads((pack / 'validation.json').read_text(encoding='utf-8'))
        assert report.get('validated'), 'Vision models must be validated before packaging'
        for name, info in report['models'].items():
            assert Path(name).name == name
            assert hashlib.sha256((pack / name).read_bytes()).hexdigest() == info['sha256']
        shutil.copytree(pack, STAGE / 'models', dirs_exist_ok=True)
        shutil.copy2(runtime / 'lib/onnxruntime.dll', STAGE)
    licenses = STAGE / 'licenses'
    licenses.mkdir(exist_ok=True)
    if (runtime / 'LICENSE').is_file():
        shutil.copy2(runtime / 'LICENSE', licenses / 'ONNXRuntime.txt')
        shutil.copy2(runtime / 'ThirdPartyNotices.txt', licenses / 'ONNXRuntime-ThirdPartyNotices.txt')
        for notice in (STAGE / 'models').glob('*LICENSE.txt'):
            shutil.copy2(notice, licenses / notice.name)
    for source, name in [(ROOT.parent / 'LICENSE', 'Compositor.txt'),
            (BUILD / '_deps/lcms-src/LICENSE', 'LittleCMS.txt'),
            (BUILD / '_deps/libtiff-src/LICENSE.md', 'libtiff.txt'),
            (BUILD / '_deps/miniz-src/LICENSE', 'miniz.txt'),
            (BUILD / '_deps/zlib-src/LICENSE', 'zlib.txt'),
            (BUILD / '_deps/jpeg_turbo-src/LICENSE.md', 'libjpeg-turbo.txt')]:
        shutil.copy2(source, licenses / name)
    shutil.copy2(BUILD / '_deps/jpeg_turbo-src/README.ijg', licenses / 'libjpeg-ijg.txt')
    shutil.copy2(BUILD / 'fonts/OFL.txt', licenses / 'NotoSansSC-OFL.txt')
    subprocess.run([str(qt / 'bin/windeployqt.exe'), '--release', '--no-compiler-runtime',
                    '--no-opengl-sw', '--no-system-d3d-compiler', '--translations', 'zh_CN,en',
                    str(STAGE / 'Yinghui.exe')], check=True)
    (STAGE / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=.\nTranslations=translations\n', encoding='utf-8')
    redist = sorted((vs / 'VC/Redist/MSVC').glob('14.*/x64/Microsoft.VC143.CRT'))[-1]
    for dll in redist.glob('*.dll'):
        shutil.copy2(dll, STAGE / dll.name)
    for folder, target in [('embedded-ai', 'ai'), ('embedded-image', 'image-ai')]:
        pack = ROOT / '.tools' / folder
        manifest = json.loads((pack / 'manifest.json').read_text(encoding='utf-8'))
        for name, info in manifest['files'].items():
            path = pack / name
            assert path.resolve().is_relative_to(pack.resolve())
            with path.open('rb') as stream:
                assert hashlib.file_digest(stream, 'sha256').hexdigest() == info['sha256']
        shutil.copytree(pack, STAGE / target, dirs_exist_ok=True, copy_function=stage_copy)
        for backend in ['cpu', 'vulkan', 'cuda']:
            directory = STAGE / target / backend
            if directory.is_dir():
                for dll in redist.glob('*.dll'):
                    shutil.copy2(dll, directory / dll.name)
        for notice in pack.glob('*LICENSE*'):
            shutil.copy2(notice, licenses / notice.name)
        for notice in pack.glob('*EULA*'):
            shutil.copy2(notice, licenses / notice.name)
        print('Staged:', target, flush=True)
    d3d = qt / 'd3dcompiler_47.dll'
    if d3d.is_file():
        shutil.copy2(d3d, STAGE / d3d.name)
    for name in ['LGPL-3.0-only', 'GPL-3.0-only', 'LicenseRef-Qt-Commercial']:
        fetch('https://raw.githubusercontent.com/qt/qtbase/v6.12.0/LICENSES/' + name + '.txt', licenses / (name + '.txt'))
    for module in ['qtbase', 'qtsvg', 'qtimageformats']:
        for path in (qt / 'sbom').glob(module + '-*.spdx*'):
            shutil.copy2(path, licenses / path.name)
    shutil.copy2(ROOT / 'THIRD-PARTY-NOTICES.md', licenses)
    shutil.copy2(ROOT / 'README.md', STAGE / 'README.md')
    shutil.copy2(ROOT / 'VALIDATION.md', STAGE / 'VALIDATION.md')
    shutil.copy2(ROOT / 'VALIDATION-0.3.1.md', STAGE / 'VALIDATION-0.3.1.md')
    shutil.copy2(ROOT / 'VALIDATION-0.4.0.md', STAGE / 'VALIDATION-0.4.0.md')
    shutil.copy2(ROOT / 'VALIDATION-0.4.1.md', STAGE / 'VALIDATION-0.4.1.md')
    shutil.copy2(ROOT / 'VALIDATION-0.4.2.md', STAGE / 'VALIDATION-0.4.2.md')
    shutil.copy2(ROOT / 'VALIDATION-0.4.3.md', STAGE / 'VALIDATION-0.4.3.md')
    shutil.copy2(ROOT / 'VALIDATION-0.4.5.md', STAGE / 'VALIDATION-0.4.5.md')
    shutil.copy2(ROOT / 'IMPLEMENTATION-0.4.0.zh-CN.md', STAGE / 'IMPLEMENTATION-0.4.0.zh-CN.md')
    shutil.copy2(ROOT / 'IMPLEMENTATION-0.4.1.zh-CN.md', STAGE / 'IMPLEMENTATION-0.4.1.zh-CN.md')
    shutil.copy2(ROOT / 'LOCAL-AI.zh-CN.md', STAGE / 'LOCAL-AI.zh-CN.md')
    shutil.copy2(ROOT / 'IMPLEMENTATION-0.4.2.zh-CN.md', STAGE / 'IMPLEMENTATION-0.4.2.zh-CN.md')
    shutil.copy2(ROOT / 'IMPLEMENTATION-0.4.3.zh-CN.md', STAGE / 'IMPLEMENTATION-0.4.3.zh-CN.md')
    shutil.copy2(ROOT / 'IMPLEMENTATION-0.4.5.zh-CN.md', STAGE / 'IMPLEMENTATION-0.4.5.zh-CN.md')
    shutil.copy2(ROOT / 'QUICKSTART.zh-CN.md', STAGE / 'QUICKSTART.zh-CN.md')
    shutil.copy2(ROOT / 'COMFYUI.zh-CN.md', STAGE / 'COMFYUI.zh-CN.md')
    shutil.copy2(ROOT / 'ComfyUI-中文修图.cmd', STAGE / 'ComfyUI-中文修图.cmd')
    (STAGE / 'scripts').mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'scripts/comfyui_client.py', STAGE / 'scripts/comfyui_client.py')
    shutil.copy2(ROOT / 'scripts/local_image_worker.py', STAGE / 'scripts/local_image_worker.py')
    shutil.copy2(ROOT / 'scripts/local_comfy_server.py', STAGE / 'scripts/local_comfy_server.py')
    pack = ROOT / '.tools/local-comfy'
    manifest = {'files': {}} if LITE else json.loads((pack / 'manifest.json').read_text(encoding='utf-8'))
    for name, info in manifest['files'].items():
        path = pack / name
        assert path.resolve().is_relative_to(pack.resolve())
        assert path.stat().st_size == info['bytes'], name
    # Staging and the private imported pack share a volume. Hard links avoid another
    # 21 GB copy; distribution archives contain real bytes, checked while writing.
    for name in ([*manifest['files'], 'manifest.json'] if not LITE else []):
        source, destination = pack / name, STAGE / 'local-qwen' / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            if os.path.samefile(source, destination): continue
            destination.unlink()
        try: os.link(source, destination)
        except OSError: shutil.copy2(source, destination)
    contents = {'version': '0.4.5', 'edition': 'lite' if LITE else 'full', 'qwen21_offline_pack': not LITE,
                'embedded_assistant': True, 'embedded_sd15': True}
    (STAGE / 'PACKAGE-CONTENTS.json').write_text(json.dumps(contents, indent=2), encoding='utf-8')
    print('Offline Qwen INT8 pack included:', not LITE, flush=True)
    (STAGE / 'assets').mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'assets/local-image-catalog.json', STAGE / 'assets/local-image-catalog.json')
    (STAGE / 'local-ai-bootstrap').mkdir(exist_ok=True)
    for name, url, expected in [
        ('python-3.12.10-embed-amd64.zip', 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip',
         '4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3'),
        ('pip.pyz', 'https://bootstrap.pypa.io/pip/pip.pyz',
         '91d5fd9f6f25549fd839c60536c6f1b945316ce3588d34a605635b6071c91526')]:
        source = ROOT / '.tools/downloads' / name
        if not source.exists(): fetch(url, source)
        with source.open('rb') as stream:
            assert hashlib.file_digest(stream, 'sha256').hexdigest() == expected
        shutil.copy2(source, STAGE / 'local-ai-bootstrap' / name)
    shutil.copytree(ROOT / 'workflows/comfyui', STAGE / 'workflows/comfyui', dirs_exist_ok=True)
    # The deployed executable must run with only Windows system directories on PATH.
    env = os.environ.copy()
    env['PATH'] = os.environ['SystemRoot'] + r'\System32;' + os.environ['SystemRoot']
    for key in ['QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH']:
        env.pop(key, None)
    subprocess.run([str(STAGE / 'Yinghui.exe'), '--validation', '--smoke-test', '--demo', '--screenshot', str(DIST / 'release-preview.png')],
                   env=env, cwd=STAGE, check=True, timeout=45)
    if (STAGE / 'models/validation.json').is_file():
        probe = STAGE / 'vision-validation.exe'
        shutil.copy2(BUILD / 'compositor_vision_smoke.exe', probe)
        try:
            subprocess.run([str(probe), str(ROOT / '.tools/vision-samples/truck.jpg'),
                str(ROOT / '.tools/vision-samples/person.jpg'), str(ROOT / 'out/vision-staged'), '-platform', 'windows'],
                env=env, cwd=STAGE, check=True, timeout=90)
        finally:
            probe.unlink()
    probe = STAGE / 'embedded-validation.exe'
    shutil.copy2(BUILD / 'compositor_embedded_smoke.exe', probe)
    try:
        subprocess.run([str(probe), '--model', 'qwen35-2b', '--backend', 'cpu', '--image', '--image-backend', 'cpu', '--output', str(ROOT / 'out/embedded-staged-045.json')],
                       env=env, cwd=STAGE, check=True, timeout=300)
    finally:
        probe.unlink()
    if not LITE:
        probe = STAGE / 'managed-validation.exe'
        shutil.copy2(BUILD / 'compositor_managed_smoke.exe', probe)
        try:
            subprocess.run([str(probe), '--root', str(ROOT / 'out/managed-staged-root'), '--output', str(ROOT / 'out/managed-staged-044.json')],
                           env=env, cwd=STAGE, check=True, timeout=600)
        finally:
            probe.unlink()
    # Low compression keeps packaging of the large, already quantized weights practical.
    import zipfile
    with zipfile.ZipFile(str(STAGE) + '.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as archive:
        for path in sorted(STAGE.rglob('*')):
            if path.is_file():
                name = path.relative_to(STAGE.parent).as_posix()
                info = zipfile.ZipInfo.from_file(path, name)
                info.compress_type = zipfile.ZIP_STORED if path.suffix in ('.safetensors', '.gguf', '.onnx') else zipfile.ZIP_DEFLATED
                info._compresslevel = 1
                sha = hashlib.sha256()
                with path.open('rb') as source, archive.open(info, 'w', force_zip64=True) as target:
                    while chunk := source.read(4 * 1024 * 1024):
                        target.write(chunk); sha.update(chunk)
                relative = path.relative_to(STAGE).as_posix()
                if relative.startswith('local-qwen/') and relative[11:] in manifest['files']:
                    assert sha.hexdigest() == manifest['files'][relative[11:]]['sha256'], relative
                if path.stat().st_size > 500 * 1024 * 1024:
                    print('Archived:', relative, flush=True)
    print('Portable ZIP complete', flush=True)
    compiler = installer_compiler()
    fetch('https://raw.githubusercontent.com/jrsoftware/issrc/is-6_7_3/Files/Languages/Unofficial/ChineseSimplified.isl', ROOT / 'installer/ChineseSimplified.isl')
    INSTALLER.mkdir(exist_ok=True)
    subprocess.run([str(compiler), '/DSourceDir=' + str(STAGE), '/DOutputDir=' + str(INSTALLER),
                    str(ROOT / 'installer/Compositor.iss')], check=True)
    finalize()
    print('Delivered:', DIST)

def finalize():
    hashes = []
    portable = ROOT / 'out/portable-044.sha256'
    receipt = ROOT / 'out/portable-044.hash-info.json'
    for path in [Path(str(STAGE) + '.zip'), *sorted(INSTALLER.glob('*'))]:
        if not path.is_file(): continue
        if path == Path(str(STAGE) + '.zip') and portable.exists() and receipt.exists():
            line = portable.read_text(encoding='utf-8').strip()
            digest, name = line.split('  ', 1)
            saved = json.loads(receipt.read_text(encoding='utf-8'))
            if name == path.name and len(digest) == 64 and saved.get('sha256') == digest and \
                    saved.get('bytes') == path.stat().st_size and saved.get('mtime_ns') == path.stat().st_mtime_ns:
                hashes.append(line)
                continue
        with path.open('rb') as stream:
            hashes.append(hashlib.file_digest(stream, 'sha256').hexdigest() + '  ' + path.relative_to(DIST).as_posix())
        print('Hashed:', path.name, flush=True)
    (DIST / 'SHA256SUMS.txt').write_text('\n'.join(hashes) + '\n', encoding='utf-8')

if __name__ == '__main__':
    import sys
    finalize() if '--finalize-only' in sys.argv else main()
