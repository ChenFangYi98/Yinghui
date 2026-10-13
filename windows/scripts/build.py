"""Reproducible MSVC build, test, and deployment entry point."""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['configure', 'build', 'test', 'package'], default='build', nargs='?')
    parser.add_argument('--debug', action='store_true')
    parser.add_argument('--clean', action='store_true', help='Rebuild all object files')
    parser.add_argument('--target', help='Build a specific target')
    args = parser.parse_args()
    preset = 'windows-debug' if args.debug else 'windows-release'
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installations = json.loads(subprocess.check_output([str(vswhere), '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-format', 'json', '-utf8'], encoding='utf-8'))
    if not installations:
        raise RuntimeError('Install Visual Studio 2022 C++ build tools')
    installation = next((i for i in installations if i['installationVersion'].startswith('17.')), installations[0])
    vs = Path(installation['installationPath'])
    cmake = shutil.which('cmake') or str(vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
    ninja = shutil.which('ninja') or str(vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe')
    ctest = str(Path(cmake).with_name('ctest.exe'))
    env = os.environ.copy()
    env['VSLANG'] = '1033'
    qt = Path(env.get('COMPOSITOR_QT_ROOT', ROOT / '.tools/Qt')).resolve()
    env['COMPOSITOR_QT_ROOT'] = str(qt)
    env['PATH'] = str(qt / 'bin') + os.pathsep + str(Path(ninja).parent) + os.pathsep + env['PATH']
    command = [cmake, '--preset', preset] if args.action == 'configure' else [cmake, '--build', '--preset', preset, '--parallel', '12']
    if args.action == 'test':
        command = [ctest, '--preset', preset]
    if args.action == 'build' and args.clean:
        command.append('--clean-first')
    if args.action == 'build' and args.target:
        command += ['--target', args.target]
    if args.action == 'package':
        subprocess.run([os.sys.executable, str(ROOT / 'scripts/package.py')], check=True, env=env)
        return
    quoted = subprocess.list2cmdline(command)
    shell = 'call "' + str(vs / 'VC/Auxiliary/Build/vcvars64.bat') + '" >nul && set "VSLANG=1033" && ' + quoted
    subprocess.run(shell, shell=True, check=True, cwd=ROOT, env=env)

if __name__ == '__main__':
    main()
