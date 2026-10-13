"""Independent Pillow/tifffile/psd-tools checks, including real Photoshop fixtures.

Run with a Python environment containing Pillow, numpy, tifffile and psd-tools.
The probe executable is a test-only target; no validation libraries ship in the app.
"""
from pathlib import Path
import json
import hashlib
import copy
import io
import uuid
import os
import subprocess
import urllib.request
import zipfile
import numpy as np
import tifffile
from PIL import Image, ImageChops, ImageCms
from psd_tools import PSDImage

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / 'out/validation'
PROBE = ROOT / 'out/release/compositor_codec_probe.exe'
ENV = os.environ.copy()
ENV['PATH'] = str(Path(ENV.get('COMPOSITOR_QT_ROOT', ROOT / '.tools/Qt')) / 'bin') + ';' + ENV['PATH']
DIRECTORY.mkdir(exist_ok=True)
RESULTS = []
FIXTURES = {
    '2layers.psd': '406ddf7cbf5a1065b992b9c0d2a59be6290a371b90f23b9a04c0e6bbc94beb0c',
    'group.psd': '1aaf572b69f0b0fa7c04b7b3a14c97e310a0c7bbdc06e733c9dd0d0cdcf7df1c',
    '1layer.psb': '85317ccdb7ec11a51f1983ad2055544a490839c01f8b313db8070c1b0f0b83f4',
}
FIXTURE_REVISION = 'b58704c1c9c9b2459f961560b1e368dfb102b513'

def probe(source, destination):
    result = subprocess.run([str(PROBE), str(source), str(destination), '-platform', 'offscreen'],
                            env=ENV, capture_output=True, timeout=60)
    if result.returncode:
        raise AssertionError((source.name, result.returncode, result.stderr.decode('utf-8', 'replace')))
    return json.loads(result.stdout)

def equal_pixels(actual, expected, tolerance=0):
    actual, expected = actual.convert('RGBA'), expected.convert('RGBA')
    assert actual.size == expected.size
    # Compare visible premultiplied color; transparent straight RGB is unspecified.
    a, b = np.asarray(actual, dtype=np.int16), np.asarray(expected, dtype=np.int16)
    for c in range(3):
        a[:, :, c] = (a[:, :, c].astype(np.int32) * a[:, :, 3] + 127) // 255
        b[:, :, c] = (b[:, :, c].astype(np.int32) * b[:, :, 3] + 127) // 255
    difference = int(np.max(np.abs(a - b)))
    assert difference <= tolerance, difference
    return difference

def main():
    rgb = np.array([[[210, 0, 0], [0, 220, 0], [0, 0, 230]],
                    [[50, 60, 70], [80, 90, 100], [110, 120, 130]]], dtype=np.uint8)
    orientations = [rgb, rgb[:, ::-1], rgb[::-1, ::-1], rgb[::-1], rgb.transpose(1, 0, 2),
                    np.rot90(rgb, -1), rgb.transpose(1, 0, 2)[::-1, ::-1], np.rot90(rgb, 1)]
    for number, expected in enumerate(orientations, 1):
        path = DIRECTORY / f'orientation-{number}.tif'
        tifffile.imwrite(path, rgb, photometric='rgb', extratags=[(274, 'H', 1, number, False)])
        output = path.with_suffix('.png')
        probe(path, output)
        equal_pixels(Image.open(output), Image.fromarray(expected))
    RESULTS.append('TIFF: all 8 orientation mappings')
    path = DIRECTORY / 'sixteen-multipage.tif'
    with tifffile.TiffWriter(path) as writer:
        writer.write(rgb.astype(np.uint16) * 257, photometric='rgb', resolution=(300, 300))
        writer.write(rgb, photometric='rgb')
    report = probe(path, path.with_suffix('.png'))
    assert len(report['conversions']) == 2 and report['resolution'] == 300
    equal_pixels(Image.open(path.with_suffix('.png')), Image.fromarray(rgb))
    RESULTS.append('TIFF: 16-bit conversion, first page, 300 DPI and conversion report')
    rgba = Image.new('RGBA', (33, 29), (120, 70, 210, 128))
    profile = ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
    source = DIRECTORY / 'alpha-icc.png'
    rgba.save(source, icc_profile=profile, dpi=(144, 144))
    tif = DIRECTORY / 'alpha-icc.tif'
    probe(source, tif)
    equal_pixels(Image.open(tif), rgba, 1)
    with tifffile.TiffFile(tif) as file:
        assert file.pages[0].compression == 5
        assert file.pages[0].extrasamples[0] == 2
        assert file.pages[0].tags[34675].value
    RESULTS.append('TIFF: independent LZW, unassociated alpha and ICC inspection')
    for version in range(1, 12):
        package = DIRECTORY / 'base.comp'
        probe(source, package)
        with zipfile.ZipFile(package) as archive:
            manifest = json.loads(archive.read('manifest.json'))
            legacy = DIRECTORY / f'legacy-v{version}.comp'
            legacy.mkdir(exist_ok=True)
            (legacy / 'images').mkdir(exist_ok=True)
            for name in archive.namelist():
                if name.startswith('images/'):
                    (legacy / name).write_bytes(archive.read(name))
        manifest['version'] = version
        manifest.pop('guides', None)
        (legacy / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
        output = DIRECTORY / f'legacy-v{version}.png'
        probe(legacy, output)
        equal_pixels(Image.open(output), rgba, 1)
    RESULTS.append('Projects: directory versions 1 through 11 import independently generated manifests')
    with zipfile.ZipFile(package) as archive:
        base_manifest = json.loads(archive.read('manifest.json'))
    for mode in ['isolated', 'passThrough', 'clipping']:
        manifest = copy.deepcopy(base_manifest)
        assets = {}
        def layer(name, color):
            value = copy.deepcopy(base_manifest['layers'][0])
            value['id'] = str(uuid.uuid4()).upper()
            value['name'] = name
            value['imageFile'] = value['id'] + '.png'
            stream = io.BytesIO()
            Image.new('RGBA', rgba.size, color).save(stream, format='PNG')
            assets['images/' + value['imageFile']] = stream.getvalue()
            return value
        background = layer('Background', (80, 110, 160, 128 if mode == 'clipping' else 255))
        top = layer('Top', (200, 40, 20, 255 if mode == 'clipping' else 180))
        top['blendMode'], top['opacity'] = 'Multiply', .75
        if mode == 'clipping':
            top['maskSourceID'] = background['id']
            manifest['layers'] = [background, top]
        else:
            group = layer('Group', (0, 0, 0, 0))
            group.pop('imageFile')
            group.update(isGroup=True, groupCompositing=mode, opacity=.55)
            top['parentID'] = group['id']
            upper = layer('Upper', (20, 120, 230, 140))
            upper['parentID'] = group['id']
            group['maskFile'] = group['id'] + '.mask.png'
            mask = Image.new('L', rgba.size, 128)
            mask.paste(255, (0, 0, 15, rgba.height))
            stream = io.BytesIO(); mask.save(stream, format='PNG')
            assets['images/' + group['maskFile']] = stream.getvalue()
            manifest['layers'] = [background, group, top, upper]
        manifest['activeLayerID'] = top['id']
        path = DIRECTORY / (mode + '.comp')
        with zipfile.ZipFile(path, 'w', allowZip64=True) as archive:
            archive.writestr('manifest.json', json.dumps(manifest))
            for name, data in assets.items(): archive.writestr(name, data)
        expected = path.with_suffix('.png'); exported = path.with_suffix('.psd')
        probe(path, expected); report = probe(path, exported)
        psd = PSDImage.open(exported)
        try:
            difference = equal_pixels(psd.composite(force=True), Image.open(expected), 4)
            merged_difference = equal_pixels(psd.composite(), Image.open(expected), 1)
        except AssertionError as error:
            raise AssertionError((mode, str(error))) from error
        RESULTS.append({'generated': mode, 'independentExportMaxError': difference,
                        'mergedMaxError': merged_difference, 'conversions': report['conversions']})
    # Force the merged-appearance path while retaining a valid translucent preview.
    unsupported = DIRECTORY / 'unsupported-blend.psd'
    encoded = (DIRECTORY / 'clipping.psd').read_bytes()
    assert encoded.count(b'8BIMnorm') == 1
    unsupported.write_bytes(encoded.replace(b'8BIMnorm', b'8BIMdiss', 1))
    output = unsupported.with_suffix('.png')
    report = probe(unsupported, output)
    assert report['layers'] == 1 and report['conversions']
    equal_pixels(Image.open(output), Image.open(DIRECTORY / 'clipping.png'), 1)
    RESULTS.append('PSD: converted merged preview preserves translucent color and alpha')
    for name, digest in FIXTURES.items():
        path = DIRECTORY / name
        if not path.is_file():
            url = f'https://raw.githubusercontent.com/psd-tools/psd-tools/{FIXTURE_REVISION}/tests/psd_files/' + name
            with urllib.request.urlopen(url, timeout=60) as response:
                path.write_bytes(response.read())
        assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, name
        expected = PSDImage.open(path).composite()
        output = DIRECTORY / (name + '.png')
        report = probe(path, output)
        error = equal_pixels(Image.open(output), expected, 3)
        exported = DIRECTORY / (name + '.export.psd')
        probe(path, exported)
        loaded = PSDImage.open(exported)
        # Force reconstruction from exported layers rather than trusting our merged preview.
        error2 = equal_pixels(loaded.composite(force=True), expected, 4)
        RESULTS.append({'fixture': name, 'layers': report['layers'], 'importMaxError': error,
                        'independentExportMaxError': error2, 'conversions': report['conversions']})
    (DIRECTORY / 'results.json').write_text(json.dumps(RESULTS, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(RESULTS, ensure_ascii=True, indent=2))

if __name__ == '__main__':
    main()
