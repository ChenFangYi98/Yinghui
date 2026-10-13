"""Install, run with an isolated PATH, and uninstall the release in the workspace."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import winreg

ROOT = Path(__file__).resolve().parents[1]
WORK = (ROOT / 'out').resolve()
WORK.relative_to(ROOT.resolve())
TARGET = (WORK / 'install-smoke').resolve()
TARGET.relative_to(WORK)
KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\{E729969A-5A89-4E46-A5C8-D925B780124C}_is1'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reuse-installer', action='store_true', help='Use the previously compiled validation installer')
parser.add_argument('--resume', action='store_true', help='Validate and uninstall a completed isolated installation')
args = parser.parse_args()
registered = False
try:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
        existing = Path(winreg.QueryValueEx(key, 'InstallLocation')[0]).resolve()
        if existing != TARGET:
            raise RuntimeError('An isolated validation installation already exists elsewhere; test skipped')
        registered = True
except FileNotFoundError:
    pass

output = (WORK / 'install-test-package').resolve()
output.relative_to(WORK)
output.mkdir(exist_ok=True)
source = ROOT / ('dist/Yinghui-Windows-0.4.5-x64' + ('-Lite' if os.environ.get('YINGHUI_PACKAGE_LITE') == '1' else ''))
compiler = ROOT / '.tools/Inno/ISCC.exe'
if not args.resume and not args.reuse_installer:
    subprocess.run([str(compiler), '/DSmokeTest=1', '/DSourceDir=' + str(source),
                    '/DOutputDir=' + str(output), str(ROOT / 'installer/Compositor.iss')], check=True)
setup = output / 'Yinghui-Windows-0.4.5-x64-Setup.exe'
flags = ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-']
if args.resume:
    if not registered or not (TARGET / 'unins000.exe').is_file():
        raise RuntimeError('No completed isolated validation installation is available to resume')
else:
    subprocess.run([str(setup), *flags, '/NOICONS', '/TASKS=', '/LANG=chinesesimplified',
                    '/DIR=' + str(TARGET), '/LOG=' + str(WORK / 'install-smoke-inno.log')],
                   check=True, timeout=7200, creationflags=subprocess.CREATE_NO_WINDOW)
try:
    # Check the installed release before launching, including every bundled model file.
    for name in ['Yinghui.exe', 'scripts/local_image_worker.py', 'scripts/local_comfy_server.py']:
        assert hashlib.sha256((TARGET / name).read_bytes()).digest() == hashlib.sha256((source / name).read_bytes()).digest(), name
    manifest_path = TARGET / 'local-qwen/manifest.json'
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        for name, info in manifest['files'].items():
            installed = (manifest_path.parent / name).resolve()
            installed.relative_to(manifest_path.parent.resolve())
            assert installed.is_file() and installed.stat().st_size == info['bytes'], name
        print('Installed offline model pack:', len(manifest['files']), 'files verified', flush=True)
    environment = os.environ.copy()
    system_root = os.environ['SystemRoot']
    environment['PATH'] = system_root + r'\System32;' + system_root
    for name in ['QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH']:
        environment.pop(name, None)
    subprocess.run([str(TARGET / 'Yinghui.exe'), '--validation', '--smoke-test', '--screenshot',
                    str(ROOT / 'dist/installed-preview.png')], cwd=TARGET, env=environment,
                   check=True, timeout=45)
finally:
    subprocess.run([str(TARGET / 'unins000.exe'), *flags], check=True, timeout=600,
                   creationflags=subprocess.CREATE_NO_WINDOW)
assert not (TARGET / 'Yinghui.exe').exists(), 'Uninstaller left the executable behind'
print('Installer, isolated application launch, and uninstall passed')
