"""Stage a complete portable release and an update for the previous full release."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / 'dist'
VERSION = re.search(r'project\(CompositorWindows VERSION ([0-9.]+)',
                    (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8'))[1]
BASE_VERSION = '0.4.3'
BASE = DIST / f'Yinghui-Windows-{BASE_VERSION}-x64'
STAGE = DIST / f'Yinghui-Windows-{VERSION}-x64'


def replace(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        target.unlink()  # Break the old-release hard link before writing updated content.
    shutil.copy2(source, target)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    assert STAGE.resolve().parent == DIST.resolve() and STAGE.resolve() != BASE.resolve()
    base_contents = json.loads((BASE / 'PACKAGE-CONTENTS.json').read_text(encoding='utf-8'))
    assert base_contents['edition'] == 'full' and base_contents['qwen21_offline_pack']
    base_digest = hashlib.sha256((BASE / 'Yinghui.exe').read_bytes()).hexdigest()
    STAGE.mkdir(exist_ok=True)
    for source in BASE.rglob('*'):
        target = STAGE / source.relative_to(BASE)
        if source.is_dir():
            target.mkdir(exist_ok=True)
        elif not target.exists():
            if source.stat().st_size < 16 * 1024 * 1024:
                shutil.copy2(source, target)
            else:
                try:
                    os.link(source, target)
                except OSError:
                    shutil.copy2(source, target)
    updated = ['Yinghui.exe', 'QUICKSTART.zh-CN.md', 'LOCAL-AI.zh-CN.md', 'README.md',
               'VALIDATION.md', 'VALIDATION-0.4.4.md', 'IMPLEMENTATION-0.4.4.zh-CN.md',
               f'VALIDATION-{VERSION}.md', f'IMPLEMENTATION-{VERSION}.zh-CN.md']
    replace(ROOT / 'out/release/Compositor.exe', STAGE / 'Yinghui.exe')
    replace(ROOT / 'QUICKSTART.zh-CN.md', STAGE / 'QUICKSTART.zh-CN.md')
    replace(ROOT / 'LOCAL-AI.zh-CN.md', STAGE / 'LOCAL-AI.zh-CN.md')
    replace(ROOT / 'README.md', STAGE / 'README.md')
    for name in ['VALIDATION-0.4.4.md', 'IMPLEMENTATION-0.4.4.zh-CN.md']:
        replace(ROOT / name, STAGE / name)
    replace(ROOT / f'IMPLEMENTATION-{VERSION}.zh-CN.md', STAGE / f'IMPLEMENTATION-{VERSION}.zh-CN.md')
    replace(ROOT / f'VALIDATION-{VERSION}.md', STAGE / 'VALIDATION.md')
    replace(ROOT / f'VALIDATION-{VERSION}.md', STAGE / f'VALIDATION-{VERSION}.md')
    model_pack = ROOT / '.tools/embedded-ai'
    assistant_manifest = json.loads((model_pack / 'manifest.json').read_text(encoding='utf-8'))
    assert any(m['id'] == 'qwen38-27b' for m in assistant_manifest['models'])
    for relative in ['manifest.json', 'Qwen3.8-LICENSE.txt',
                     'models/qwen38/Qwen3.8-27B-UD-Q4_K_M.gguf', 'models/qwen38/mmproj-F16.gguf']:
        source, target = model_pack / relative, STAGE / 'ai' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists(): target.unlink()
        if source.stat().st_size > 16 * 1024 * 1024:
            os.link(source, target)
        else:
            shutil.copy2(source, target)
        updated.append('ai/' + relative)
        if relative != 'manifest.json':
            assert digest(target) == assistant_manifest['files'][relative]['sha256']
    replace(ROOT / 'THIRD-PARTY-NOTICES.md', STAGE / 'THIRD-PARTY-NOTICES.md')
    updated.append('THIRD-PARTY-NOTICES.md')
    contents = {**base_contents, 'version': VERSION, 'base_runtime_version': BASE_VERSION,
                'qwen38_27b_offline_pack': True}
    target = STAGE / 'PACKAGE-CONTENTS.json'
    if target.exists(): target.unlink()
    target.write_text(json.dumps(contents, indent=2), encoding='utf-8')
    updated.append('PACKAGE-CONTENTS.json')
    hashes = {name: digest(STAGE / name) for name in updated}
    manifest = json.loads((STAGE / 'local-qwen/manifest.json').read_text(encoding='utf-8'))
    for name, info in manifest['files'].items():
        path = STAGE / 'local-qwen' / name
        assert path.resolve().is_relative_to((STAGE / 'local-qwen').resolve())
        assert path.is_file() and path.stat().st_size == info['bytes'], name
    assert hashlib.sha256((BASE / 'Yinghui.exe').read_bytes()).hexdigest() == base_digest
    assert base_digest != hashes['Yinghui.exe']
    archive = DIST / f'Yinghui-Update-{BASE_VERSION}-to-{VERSION}-x64.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, allowZip64=True) as output:
        for name in updated:
            entry = zipfile.ZipInfo.from_file(STAGE / name, name)
            entry.compress_type = zipfile.ZIP_STORED if name.endswith('.gguf') else zipfile.ZIP_DEFLATED
            print('Archiving', name, flush=True)
            # Large buffers avoid excessive seeks when reading and writing on the same disk.
            with (STAGE / name).open('rb') as source, output.open(entry, 'w', force_zip64=True) as destination:
                shutil.copyfileobj(source, destination, 8 * 1024 * 1024)
        output.writestr('UPDATE.json', json.dumps({'base_version': BASE_VERSION, 'version': VERSION,
            'required_base_file': 'local-qwen/manifest.json', 'sha256': hashes}, indent=2))
        output.writestr('UPDATE.zh-CN.txt',
            '映绘更新包：需要已有 0.4.3 或 0.4.4 完整版。\n先退出软件，将本 ZIP 的文件覆盖到包含 Yinghui.exe 的程序目录。\n'
            '保留原有模型和运行库，再运行 Yinghui.exe。模型设置、凭据和项目不会被清除。\n'
            '本 ZIP 包含新的 Qwen3.8 看图权重，不能单独运行。安装器版本记录仍为基础安装包版本。\n')
    with zipfile.ZipFile(archive) as output:
        for name, expected in hashes.items():
            with output.open(name) as stream:
                assert hashlib.file_digest(stream, 'sha256').hexdigest() == expected
    env = os.environ.copy()
    env['PATH'] = os.environ['SystemRoot'] + r'\System32;' + os.environ['SystemRoot']
    for key in ['QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH']:
        env.pop(key, None)
    subprocess.run([str(STAGE / 'Yinghui.exe'), '--validation', '--smoke-test', '--demo',
        '--screenshot', str(DIST / f'release-{VERSION}-preview.png')], cwd=STAGE, env=env,
        check=True, timeout=45)
    result = {'version': VERSION, 'status': 'passed', 'portable_directory': str(STAGE),
        'update_zip': str(archive), 'sha256': digest(archive),
        'updated_files': hashes, 'offline_model_files': len(manifest['files']),
        'base_release_unchanged': True, 'isolated_system_path_launch': True}
    (ROOT / 'out/update-045.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    (DIST / f'SHA256SUMS-{VERSION}-Update.txt').write_text(result['sha256'] + '  ' + archive.name + '\n', encoding='utf-8')
    print('Portable release and update verified:', STAGE, archive)


if __name__ == '__main__':
    main()
