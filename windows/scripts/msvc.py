"""Normalize localized MSVC output so Ninja can track headers on Chinese Windows."""
import os
import subprocess
import sys

result = subprocess.run(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
for destination, output in [(sys.stdout, result.stdout), (sys.stderr, result.stderr)]:
    if os.name == 'nt':
        try:
            decoded = output.decode('utf-8')
        except UnicodeDecodeError:
            decoded = output.decode('mbcs', errors='replace')
        output = decoded.replace('\u6ce8\u610f: \u5305\u542b\u6587\u4ef6:',
                                 'Note: including file:').encode('utf-8')
    destination.buffer.write(output)
    destination.flush()
sys.exit(result.returncode)
