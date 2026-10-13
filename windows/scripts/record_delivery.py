"""Record release evidence after packaging and the isolated installation check."""
from pathlib import Path
import json
import hashlib
import zipfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out'
DIST = ROOT / 'dist'


def read(path):
    data = path.read_bytes()
    return data.decode('utf-16' if data.startswith(b'\xff\xfe') else 'utf-8-sig')


def evidence(name):
    data = json.loads(read(OUT / name))
    assert data.get('status') == 'passed', name
    return data


def main():
    reports = {name: evidence(name) for name in ['managed-quant-043.json', 'managed-1024-043.json',
        'managed-handoff-043.json', 'managed-staged-043.json', 'private-runtime-cpu-043.json',
        'embedded-staged-043.json']}
    assert reports['managed-handoff-043.json']['versions_undo_redo_passed']
    assert all(reports[name]['outside_mask_unchanged'] and reports[name]['real_inference_cancel_passed']
               for name in ['managed-quant-043.json', 'managed-1024-043.json'])
    native = 0
    for path in (OUT / 'release').glob('*-tests.xml'):
        for suite in ET.parse(path).getroot().iter('testsuite'):
            assert int(suite.get('failures', 0)) == 0 and int(suite.get('errors', 0)) == 0, path
            native += int(suite.get('tests', 0)) - 2
    assert native == 116, native
    assert 'Ran 22 tests' in read(OUT / 'python-tests-043.log') and 'OK' in read(OUT / 'python-tests-043.log')
    assert 'Installer, isolated application launch, and uninstall passed' in read(OUT / 'install-043.log')
    artifacts = []
    for line in read(DIST / 'SHA256SUMS.txt').splitlines():
        digest, name = line.split('  ', 1)
        path = DIST / name
        assert len(digest) == 64 and path.is_file() and path.resolve().is_relative_to(DIST.resolve()), name
        artifacts.append({'path': name, 'bytes': path.stat().st_size, 'sha256': digest})
    assert any(a['path'] == 'Yinghui-Windows-0.4.3-x64.zip' for a in artifacts)
    assert any(a['path'].endswith('Setup.exe') for a in artifacts)
    assert any(a['path'].endswith('.bin') for a in artifacts)
    prefix = 'Yinghui-Windows-0.4.3-x64/'
    with zipfile.ZipFile(DIST / 'Yinghui-Windows-0.4.3-x64.zip') as archive:
        for name, source in [('Yinghui.exe', OUT / 'release/Compositor.exe'),
                             ('scripts/local_image_worker.py', ROOT / 'scripts/local_image_worker.py'),
                             ('scripts/local_comfy_server.py', ROOT / 'scripts/local_comfy_server.py')]:
            assert hashlib.sha256(archive.read(prefix + name)).digest() == hashlib.sha256(source.read_bytes()).digest(), name
        contents = json.loads(archive.read(prefix + 'PACKAGE-CONTENTS.json'))
        assert contents['qwen21_offline_pack'] and contents['edition'] == 'full'
        manifest = json.loads(archive.read(prefix + 'local-qwen/manifest.json'))
        for name, info in manifest['files'].items():
            assert archive.getinfo(prefix + 'local-qwen/' + name).file_size == info['bytes'], name
    result = {'version': '0.4.3', 'display_name': '映绘 Yinghui', 'status': 'passed',
              'native_tests': native, 'python_tests': 22,
              'ui_scenarios': len(json.loads(read(OUT / 'ui-validation/results.json'))),
              'installed_launched_uninstalled': True, 'model_license': 'Qwen Research License',
              'evidence': reports, 'artifacts': artifacts,
              'not_verified': ['Windows 10 / clean VM', 'Qwen 2.1 full CPU inference',
                               'Qwen-Image-Edit-2511 original-weight inference', 'cloud AI services']}
    (OUT / 'delivery-043.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Validated Yinghui 0.4.3 delivery:', native, 'native tests, 22 Python tests,', len(artifacts), 'artifacts')


if __name__ == '__main__':
    main()
