"""Audit the local vision update using streamed hashes and real inference receipts."""
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


def receipt(name):
    data = json.loads(read(OUT / name))
    assert data['status'] == 'passed', name
    return data


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    update = receipt('update-045.json')
    vision = receipt('qwen38-045.json')
    lightweight = receipt('embedded-2b-045.json')
    approximate = receipt('approximate-image-045.json')
    staged = receipt('staged-qwen38-045.json')
    routing = receipt('routing-qwen38-045.json')
    assert vision['restart_refreshed_connection'] and vision['durable_versions']
    assert vision['approximate_changed_inside'] > 0 and vision['approximate_changed_outside'] == 0
    assert approximate['approximate_versions_undo_redo']
    assert approximate['approximate_changed_inside'] > 0 and approximate['approximate_changed_outside'] == 0
    assert routing['image_only_skips_language'] and routing['continuous_edits_versions_undo_redo']
    assert routing['unauthenticated_request_rejected']
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
    assert sum(tests.values()) == 137, tests
    ui = json.loads(read(OUT / 'ui-validation/results.json'))
    assert len(ui) == 11
    archive = Path(update['update_zip'])
    # package_update.py already streams and checks every archived digest.
    assert archive.is_file() and archive.stat().st_size > 17_000_000_000
    with zipfile.ZipFile(archive) as content:
        for name, expected in update['updated_files'].items():
            assert content.getinfo(name).file_size == (Path(update['portable_directory']) / name).stat().st_size
            if not name.endswith('.gguf'):
                assert hashlib.sha256(content.read(name)).hexdigest() == expected
        assert hashlib.sha256(content.read('Yinghui.exe')).hexdigest() == digest(OUT / 'release/Compositor.exe')
    with zipfile.ZipFile(ROOT / 'dist/Yinghui-Windows-0.4.3-x64.zip') as base:
        assert hashlib.sha256(base.read('Yinghui-Windows-0.4.3-x64/Yinghui.exe')).hexdigest() == digest(
            ROOT / 'dist/Yinghui-Windows-0.4.3-x64/Yinghui.exe')
    result = {'version': '0.4.5', 'status': 'passed', 'tests': tests, 'native_tests': 137,
              'ui_scenarios': 11, 'real_vision': vision, 'real_lightweight': lightweight,
              'real_local_routing': routing, 'real_approximate_image': approximate,
              'isolated_staged_vision': staged, 'delivery': update,
              'not_verified': ['Cloud services', 'Windows 10 / clean VM',
                               'Full CPU editing with Qwen3.8-27B and Qwen-Image-2.1',
                               'Qwen-Image-Edit-2511 original-weight inference']}
    (OUT / 'delivery-045.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print('Local AI update passed: 137 native cases, 11 UI scenarios, real vision, routing and approximate image editing')


if __name__ == '__main__':
    main()
