"""Install the pinned Qt SDK into the workspace without changing system settings."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import hashlib
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / '.tools'
VERSION = '6.12.0'
REPOSITORY = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/qt6_6120_msvc2022_64/'

def download(url, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(url, timeout=120) as response, path.open('wb') as output:
        while data := response.read(1024 * 1024):
            output.write(data)

def main():
    python_packages = TOOLS / 'python'
    sys.path.insert(0, str(python_packages))
    try:
        import py7zr
    except ImportError:
        subprocess.run([sys.executable, '-m', 'pip', 'install', '--target', str(python_packages), 'py7zr==1.1.3'], check=True)
        import py7zr
    with urllib.request.urlopen(REPOSITORY + 'Updates.xml', timeout=60) as response:
        root = ET.fromstring(response.read())
    packages = []
    for package in root.findall('PackageUpdate'):
        name = package.findtext('Name', '')
        if name not in ('qt.qt6.6120.win64_msvc2022_64', 'qt.qt6.6120.addons.qtimageformats.win64_msvc2022_64'):
            continue
        for archive in package.findtext('DownloadableArchives', '').split(','):
            archive = archive.strip()
            if not archive.startswith(('qtbase-', 'qtsvg-', 'qttools-', 'qttranslations-', 'qtimageformats-', 'd3dcompiler_')):
                continue
            url = REPOSITORY + name + '/' + package.findtext('Version') + archive
            packages.append((url, TOOLS / 'downloads' / archive))
    if len(packages) < 5:
        raise RuntimeError('Required Qt modules are absent from the official SDK index')
    def fetch(item):
        url, path = item
        with urllib.request.urlopen(url + '.sha256', timeout=60) as response:
            digest = response.read().decode().split()[0]
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            print('Downloading', path.name, flush=True)
            download(url, path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError('Checksum mismatch: ' + path.name)
        return path
    with ThreadPoolExecutor(max_workers=4) as pool:
        archives = list(pool.map(fetch, packages))
    for archive in archives:
        print('Extracting', archive.name, flush=True)
        with py7zr.SevenZipFile(archive) as package:
            package.extractall(TOOLS / 'Qt')
    qt_root = TOOLS / 'Qt'
    (qt_root / 'bin' / 'qt.conf').write_text('[Paths]\nPrefix=..\n', encoding='utf-8')
    print('COMPOSITOR_QT_ROOT=' + str(qt_root), flush=True)

if __name__ == '__main__':
    main()
