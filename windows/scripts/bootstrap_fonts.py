"""Install the pinned, OFL-licensed Chinese fallback font into the workspace."""
from pathlib import Path
import hashlib
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REVISION = '518dd7e35e026a565d6f655413c4ebe120d11e87'
FILES = [
    ('NotoSansSC%5Bwght%5D.ttf', 'NotoSansSC.ttf', 'a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da'),
    ('OFL.txt', 'OFL.txt', '1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9'),
]

def install():
    directory = ROOT / '.tools/fonts'
    directory.mkdir(parents=True, exist_ok=True)
    for source, name, digest in FILES:
        path = directory / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            url = f'https://raw.githubusercontent.com/google/fonts/{REVISION}/ofl/notosanssc/{source}'
            with urllib.request.urlopen(url, timeout=60) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != digest:
                raise RuntimeError('Font checksum mismatch: ' + name)
            path.write_bytes(data)

if __name__ == '__main__':
    install()
