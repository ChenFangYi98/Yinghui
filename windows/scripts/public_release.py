"""Create a source archive and audit a public release folder without user data."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import zipfile

ROOT = Path(__file__).resolve().parents[2]
WINDOWS = ROOT / 'windows'
DELIVERY = WINDOWS / 'dist/Yinghui-0.4.5-Open-Source-Delivery'
SOURCE_ZIP = DELIVERY / 'Yinghui-Source-0.4.5.zip'
INSTALLER_ZIP = DELIVERY / 'Yinghui-Windows-0.4.5-x64-Offline-Installer.zip'
PORTABLE_ZIP = DELIVERY / 'Yinghui-Windows-0.4.5-x64-Portable-with-Source.zip'
PORTABLE_FOLDER = DELIVERY / 'Yinghui-Windows-0.4.5-x64-Portable'
ARCHIVE_ROOT = 'Yinghui-Source-0.4.5'
PORTABLE = WINDOWS / 'dist/Yinghui-Windows-0.4.5-x64'
INSTALL_STAGE = WINDOWS / 'out/install-stage-045'

DIRECTORIES = [
    '.github', 'Compositor', 'Compositor.xcodeproj', 'CompositorTests',
    'CompositorUITests', 'Config', 'docs', 'scripts',
    'windows/assets', 'windows/installer', 'windows/publication',
    'windows/scripts', 'windows/src', 'windows/tests', 'windows/workflows',
]
ROOT_FILES = ['.gitignore', 'AGENTS.md', 'LICENSE', 'README.md', 'appcast.xml']
WINDOWS_FILES = ['.clang-format', 'benchmark.json', 'CMakeLists.txt',
                 'CMakePresets.json', 'ComfyUI-中文修图.cmd']
EXCLUDED_PARTS = {'__pycache__', '.git', '.cache', '.pytest_cache',
                  'xcuserdata', 'DerivedData', 'dist', 'out', '.tools'}
EXCLUDED_SUFFIXES = {'.pyc', '.pyo', '.log', '.tmp', '.bak', '.p12', '.p8', '.pem', '.key'}
TEXT_SUFFIXES = {'.c', '.cpp', '.h', '.hpp', '.swift', '.py', '.md', '.html', '.txt',
                 '.json', '.yml', '.yaml', '.iss', '.isl', '.plist', '.xml',
                 '.sh', '.cmd', '.rc', '.svg', '.qrc', '.pbxproj', '.xcscheme',
                 '.entitlements', '.cmake', '.ini'}
SECRET_PATTERNS = [
    re.compile(r'sk-[A-Za-z0-9_.-]{20,}'),
    re.compile(r'AIza[A-Za-z0-9_-]{30,}'),
    re.compile(r'gh[pousr]_[A-Za-z0-9]{20,}'),
    re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_files():
    files = []
    for relative in ROOT_FILES:
        path = ROOT / relative
        assert path.is_file(), path
        files.append(path)
    for relative in WINDOWS_FILES:
        path = WINDOWS / relative
        assert path.is_file(), path
        files.append(path)
    for path in WINDOWS.glob('*.md'):
        files.append(path)
    for relative in DIRECTORIES:
        directory = ROOT / relative
        assert directory.is_dir(), directory
        for path in directory.rglob('*'):
            if not path.is_file():
                continue
            subpath = path.relative_to(ROOT)
            if any(part in EXCLUDED_PARTS for part in subpath.parts) or path.suffix.lower() in EXCLUDED_SUFFIXES:
                continue
            files.append(path)
    return sorted(set(files), key=lambda path: path.relative_to(ROOT).as_posix())


def inspect_source(path):
    relative = path.relative_to(ROOT).as_posix()
    if path.stat().st_size > 32 * 1024 * 1024:
        raise RuntimeError('Unexpectedly large source file: ' + relative)
    if path.suffix.lower() not in TEXT_SUFFIXES and path.name not in {'LICENSE', 'README.md', '.gitignore'}:
        return
    data = path.read_bytes()
    try:
        content = data.decode('utf-8')
    except UnicodeDecodeError:
        if path.suffix.lower() in TEXT_SUFFIXES:
            raise RuntimeError('Source text is not UTF-8: ' + relative)
        return
    if any(pattern.search(content) for pattern in SECRET_PATTERNS):
        raise RuntimeError('Potential credential in source file: ' + relative)


def build_source():
    DELIVERY.mkdir(parents=True, exist_ok=True)
    files = source_files()
    for path in files:
        inspect_source(path)
    assert (WINDOWS / 'src/ai/EmbeddedAI.cpp') in files
    assert (WINDOWS / 'scripts/bootstrap_qwen38.py') in files
    assert (WINDOWS / 'publication/源码包说明.zh-CN.md') in files
    with zipfile.ZipFile(SOURCE_ZIP, 'w', compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9, allowZip64=True) as archive:
        for path in files:
            relative = path.relative_to(ROOT).as_posix()
            archive.write(path, ARCHIVE_ROOT + '/' + relative)
    with zipfile.ZipFile(SOURCE_ZIP) as archive:
        assert archive.testzip() is None
        names = set(archive.namelist())
        assert len(names) == len(files)
        assert ARCHIVE_ROOT + '/LICENSE' in names
        assert ARCHIVE_ROOT + '/windows/publication/使用手册.zh-CN.md' in names
        assert all('/.tools/' not in name and '/dist/' not in name and '/out/' not in name
                   for name in names)
    print('Source archive passed:', len(files), 'files,', SOURCE_ZIP.stat().st_size, 'bytes')


def build_install_stage():
    assert PORTABLE.is_dir() and (PORTABLE / 'Yinghui.exe').is_file()
    assert INSTALL_STAGE.resolve().is_relative_to((WINDOWS / 'out').resolve())
    INSTALL_STAGE.mkdir(parents=True, exist_ok=True)
    count = 0
    for source in PORTABLE.rglob('*'):
        target = INSTALL_STAGE / source.relative_to(PORTABLE)
        if source.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(source, target)
            except OSError:
                shutil.copy2(source, target)
            count += 1
    for name in ['README.md', 'QUICKSTART.zh-CN.md', 'LOCAL-AI.zh-CN.md',
                 'IMPLEMENTATION-0.4.5.zh-CN.md', 'VALIDATION-0.4.5.md', 'VALIDATION.md']:
        target = INSTALL_STAGE / name
        target.unlink(missing_ok=True)  # Break the base release's hard link.
        shutil.copy2(WINDOWS / name, target)
    for source in (WINDOWS / 'publication').glob('*'):
        if source.is_file():
            target = INSTALL_STAGE / source.name
            target.unlink(missing_ok=True)
            shutil.copy2(source, target)
    assert json.loads((INSTALL_STAGE / 'PACKAGE-CONTENTS.json').read_text(encoding='utf-8'))['version'] == '0.4.5'
    assert (INSTALL_STAGE / 'ai/models/qwen38/Qwen3.8-27B-UD-Q4_K_M.gguf').stat().st_size == 16_464_440_224
    print('Installer stage ready:', count, 'linked files; corrected current documentation')


def bundle_installer():
    """Offer one offline download; extraction is still needed before Setup.exe runs."""
    setup = DELIVERY / 'Yinghui-Windows-0.4.5-x64-Setup.exe'
    parts = [DELIVERY / f'Yinghui-Windows-0.4.5-x64-Setup-{index}.bin'
             for index in range(1, 22)]
    assert setup.is_file() and all(path.is_file() for path in parts)
    assert sum(path.stat().st_size for path in parts) > 38_000_000_000
    with zipfile.ZipFile(INSTALLER_ZIP, 'w', compression=zipfile.ZIP_STORED,
                         allowZip64=True) as archive:
        archive.writestr('安装说明.txt',
                         '映绘 Yinghui 0.4.5 完整离线安装包\n'
                         '先将此 ZIP 全部解压到同一文件夹，再双击 '
                         'Yinghui-Windows-0.4.5-x64-Setup.exe。\n'
                         '安装时 Setup.exe 和全部 21 个 .bin 文件必须保持在一起。\n')
        for path in [setup, *parts]:
            info = zipfile.ZipInfo.from_file(path, path.name)
            info.compress_type = zipfile.ZIP_STORED
            with path.open('rb') as source, archive.open(info, 'w', force_zip64=True) as target:
                shutil.copyfileobj(source, target, 8 * 1024 * 1024)
    with zipfile.ZipFile(INSTALLER_ZIP) as archive:
        assert len(archive.namelist()) == 23
        assert archive.testzip() is None
    print('Single-download installer archive passed:', INSTALLER_ZIP.stat().st_size,
          'bytes', flush=True)


def bundle_portable():
    """Bundle a directly runnable portable folder with the public source archive."""
    assert SOURCE_ZIP.is_file()
    assert (INSTALL_STAGE / 'Yinghui.exe').is_file()
    assert (INSTALL_STAGE / 'ai/models/qwen38/Qwen3.8-27B-UD-Q4_K_M.gguf').is_file()
    files = sorted((path for path in INSTALL_STAGE.rglob('*') if path.is_file()),
                   key=lambda path: path.relative_to(INSTALL_STAGE).as_posix())
    assert len(files) > 35_000
    assert sum(path.stat().st_size for path in files) > 44_000_000_000
    prefix = 'Yinghui-Windows-0.4.5-x64-Portable'
    with zipfile.ZipFile(PORTABLE_ZIP, 'w', compression=zipfile.ZIP_STORED,
                         allowZip64=True) as archive:
        archive.writestr('便携版使用说明.txt',
                         '映绘 Yinghui 0.4.5 便携版 + 源码\n'
                         f'完整解压后，双击 {prefix}\\Yinghui.exe 即可运行，无需安装。\n'
                         '请保留解压后的所有子文件夹和模型文件。\n'
                         '源码见同一压缩包根目录的 Yinghui-Source-0.4.5.zip。\n')
        for path in [SOURCE_ZIP, *files]:
            if path == SOURCE_ZIP:
                name = path.name
            else:
                name = prefix + '/' + path.relative_to(INSTALL_STAGE).as_posix()
            info = zipfile.ZipInfo.from_file(path, name)
            info.compress_type = zipfile.ZIP_STORED
            with path.open('rb') as source, archive.open(info, 'w', force_zip64=True) as target:
                shutil.copyfileobj(source, target, 8 * 1024 * 1024)
    with zipfile.ZipFile(PORTABLE_ZIP) as archive:
        assert len(archive.namelist()) == len(files) + 2
        assert prefix + '/Yinghui.exe' in archive.namelist()
        assert 'Yinghui-Source-0.4.5.zip' in archive.namelist()
        assert archive.testzip() is None
    print('Portable and source archive passed:', len(files), 'runtime files,',
          PORTABLE_ZIP.stat().st_size, 'bytes', flush=True)


def portable_folder():
    """Expose the validated portable build for immediate execution in the delivery folder."""
    assert INSTALL_STAGE.is_dir()
    assert PORTABLE_FOLDER.resolve().is_relative_to(DELIVERY.resolve())
    PORTABLE_FOLDER.mkdir(parents=True, exist_ok=True)
    count = 0
    for source in INSTALL_STAGE.rglob('*'):
        target = PORTABLE_FOLDER / source.relative_to(INSTALL_STAGE)
        if source.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(source, target)
            except OSError:
                shutil.copy2(source, target)
            count += 1
    assert (PORTABLE_FOLDER / 'Yinghui.exe').is_file()
    assert (PORTABLE_FOLDER / 'ai/models/qwen38/Qwen3.8-27B-UD-Q4_K_M.gguf').is_file()
    print('Direct-run portable folder ready:', count, 'new links', flush=True)


def finalize():
    assert SOURCE_ZIP.is_file()
    for source in sorted((WINDOWS / 'publication').glob('*')):
        if source.suffix not in {'.md', '.html'}:
            continue
        shutil.copy2(source, DELIVERY / source.name)
    shutil.copy2(WINDOWS / 'publication/宣传配图-软件界面.png', DELIVERY / '宣传配图-软件界面.png')
    setup = DELIVERY / 'Yinghui-Windows-0.4.5-x64-Setup.exe'
    parts = sorted(DELIVERY.glob('Yinghui-Windows-0.4.5-x64-Setup-*.bin'))
    assert setup.is_file() and setup.stat().st_size > 1_000_000
    assert len(parts) >= 15 and all(path.stat().st_size > 1_000_000 for path in parts)
    assert sum(path.stat().st_size for path in parts) > 38_000_000_000
    if INSTALLER_ZIP.exists():
        assert INSTALLER_ZIP.stat().st_size > 38_000_000_000
    if PORTABLE_ZIP.exists():
        assert PORTABLE_ZIP.stat().st_size > 44_000_000_000
    if PORTABLE_FOLDER.exists():
        assert (PORTABLE_FOLDER / 'Yinghui.exe').is_file()
    files = [setup, *parts, SOURCE_ZIP, *sorted(DELIVERY.glob('*.md')),
             *sorted(DELIVERY.glob('*.html')),
             DELIVERY / '宣传配图-软件界面.png']
    if INSTALLER_ZIP.exists():
        files.append(INSTALLER_ZIP)
    if PORTABLE_ZIP.exists():
        files.append(PORTABLE_ZIP)
    hashes = {}
    for path in files:
        hashes[path.name] = digest(path)
        print('Verified', path.name, flush=True)
    (DELIVERY / 'SHA256SUMS.txt').write_text(
        ''.join(f'{sha}  {name}\n' for name, sha in hashes.items()), encoding='utf-8')
    receipt = {'version': '0.4.5', 'source_files': len(source_files()),
               'installer_parts': len(parts), 'installer_bytes': sum(p.stat().st_size for p in [setup, *parts]),
               'files': {name: {'bytes': (DELIVERY / name).stat().st_size, 'sha256': sha}
                         for name, sha in hashes.items()}}
    (DELIVERY / '交付清单.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Public delivery passed:', DELIVERY)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['source', 'install-stage', 'bundle-installer',
                                           'bundle-portable', 'portable-folder', 'finalize'])
    args = parser.parse_args()
    if args.action == 'source':
        build_source()
    elif args.action == 'install-stage':
        build_install_stage()
    elif args.action == 'bundle-installer':
        bundle_installer()
    elif args.action == 'bundle-portable':
        bundle_portable()
    elif args.action == 'portable-folder':
        portable_folder()
    else:
        finalize()
