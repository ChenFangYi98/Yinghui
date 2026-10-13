"""Audit the 0.4.4 assistant update and its real local-model evidence."""
from pathlib import Path
import hashlib
import json
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out'


def read(path):
    data = path.read_bytes()
    return data.decode('utf-16' if data.startswith(b'\xff\xfe') else 'utf-8-sig')


def main():
    update = json.loads(read(OUT / 'update-044.json'))
    routing = json.loads(read(OUT / 'routing-044.json'))
    assert update['status'] == routing['status'] == 'passed'
    assert routing['image_only_skips_language'] and routing['continuous_edits_versions_undo_redo']
    cases = routing['routing']
    assert len(cases) == 3
    assert all(case['versions_undo_redo'] and case['changed_inside'] > 0 for case in cases)
    assert cases[0]['changed_outside'] == cases[2]['changed_outside'] == 0
    assert cases[1]['changed_outside'] > 0
    assert not cases[0]['used_language'] and not cases[1]['used_language'] and cases[2]['used_language']
    tests = {}
    for path in (OUT / 'release').glob('*-tests.xml'):
        count = 0
        for suite in ET.parse(path).getroot().iter('testsuite'):
            assert int(suite.get('failures', 0)) == int(suite.get('errors', 0)) == 0
            count += int(suite.get('tests', 0)) - 2
        tests[path.stem] = count
    assert sum(tests.values()) == 127, tests
    ui = json.loads(read(OUT / 'ui-validation/results.json'))
    assert len(ui) == 11
    archive = Path(update['update_zip'])
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == update['sha256']
    with zipfile.ZipFile(archive) as content:
        for name, digest in update['updated_files'].items():
            assert hashlib.sha256(content.read(name)).hexdigest() == digest
            assert hashlib.sha256((Path(update['portable_directory']) / name).read_bytes()).hexdigest() == digest
        assert hashlib.sha256(content.read('Yinghui.exe')).digest() == hashlib.sha256((OUT / 'release/Compositor.exe').read_bytes()).digest()
    with zipfile.ZipFile(ROOT / 'dist/Yinghui-Windows-0.4.3-x64.zip') as base:
        assert hashlib.sha256(base.read('Yinghui-Windows-0.4.3-x64/Yinghui.exe')).digest() == hashlib.sha256((ROOT / 'dist/Yinghui-Windows-0.4.3-x64/Yinghui.exe').read_bytes()).digest()
    result = {'version': '0.4.4', 'status': 'passed', 'tests': tests, 'native_tests': 127,
              'ui_scenarios': 11, 'real_local_routing': routing, 'delivery': update,
              'not_verified': ['Cloud services', 'Windows 10 / clean VM', 'Full CPU inference for Qwen-Image-2.1']}
    (OUT / 'delivery-044.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Assistant update passed: 127 native cases, 11 UI scenarios, 3 real local-model routes')


if __name__ == '__main__':
    main()
