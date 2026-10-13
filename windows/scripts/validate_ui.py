"""Capture native home and simple-workspace layouts at several display scales."""
from pathlib import Path
import json
import os
import subprocess
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'out/ui-validation'
OUTPUT.mkdir(exist_ok=True)
results = []
for name, scale, size, content in [('home', '1', '1366x768', '--home'),
        ('simple', '1', '1366x768', '--demo'),
        ('simple-125', '1.25', '1090x610', '--demo'),
        ('simple-150', '1.5', '900x510', '--demo'),
        ('connections', '1', '1000x740', '--connections'),
        ('connections-125', '1.25', '1090x610', '--connections'),
        ('connections-150', '1.5', '900x510', '--connections'),
        ('local-ai', '1', '1120x840', '--local-ai'),
        ('local-ai-150', '1.5', '900x510', '--local-ai'),
        ('managed-ai', '1', '1120x840', '--managed-ai'),
        ('managed-ai-150', '1.5', '900x510', '--managed-ai')]:
    env = os.environ.copy()
    env['PATH'] = str(ROOT / '.tools/Qt/bin') + os.pathsep + env['PATH']
    env['QT_SCALE_FACTOR'] = scale
    path = OUTPUT / (name + '.png')
    subprocess.run([str(ROOT / 'out/release/Compositor.exe'), '--validation', '--smoke-test',
        content, '--window-size', size, '--screenshot', str(path)], env=env, check=True, timeout=40)
    image = Image.open(path)
    width, height = map(int, size.split('x'))
    if content in ('--connections', '--local-ai', '--managed-ai'):
        height -= 40
    assert abs(image.width - width * float(scale)) <= 2, (name, image.size)
    assert abs(image.height - height * float(scale)) <= 2, (name, image.size)
    results.append({'scenario': name, 'scale': float(scale), 'requestedLogicalSize': size,
                    'capturedPixels': image.size})
(OUTPUT / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
print(json.dumps(results, indent=2))
