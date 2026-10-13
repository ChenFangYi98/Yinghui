"""Check the private copied runtime without loading models or using a user's Comfy service."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import urllib.request
import urllib.error
import uuid

parser = argparse.ArgumentParser()
parser.add_argument('--pack', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cpu', action='store_true')
args = parser.parse_args()
pack = args.pack.resolve()
root = args.output.resolve().parent / 'private-runtime-probe'
for name in ['input', 'output', 'user', 'temp']: (root / name).mkdir(parents=True, exist_ok=True)
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
token = uuid.uuid4().hex
env = os.environ.copy()
for key in ['PYTHONPATH', 'PYTHONHOME', 'HF_TOKEN', 'HUGGING_FACE_HUB_TOKEN', 'HF_ENDPOINT']:
    env.pop(key, None)
env.update(YINGHUI_LOCAL_TOKEN=token, PYTHONUTF8='1', PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1', HF_HUB_OFFLINE='1')
env['PATH'] = str(pack / 'runtime') + ';' + os.environ['SystemRoot'] + '/System32'
launcher = Path(__file__).resolve().parents[1] / 'scripts/local_comfy_server.py'
command = [str(pack / 'runtime/python.exe'), str(launcher), str(pack / 'comfy'), '--listen', '127.0.0.1',
           '--port', str(port), '--base-directory', str(root), '--models-directory', str(pack / 'models'),
           '--input-directory', str(root / 'input'), '--output-directory', str(root / 'output'),
           '--user-directory', str(root / 'user'), '--temp-directory', str(root / 'temp'),
           '--disable-auto-launch', '--disable-all-custom-nodes', '--disable-api-nodes', '--disable-metadata']
if args.cpu: command.append('--cpu')
report = {'status': 'failed', 'cpu_requested': args.cpu, 'models_loaded': False}
with args.output.with_suffix('.engine.log').open('w', encoding='utf-8') as log:
    process = subprocess.Popen(command, cwd=pack / 'comfy', env=env, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        start = time.monotonic()
        while time.monotonic() - start < 600:
            if process.poll() is not None: raise RuntimeError('Private engine exited; see engine log')
            try:
                request = urllib.request.Request(f'http://127.0.0.1:{port}/system_stats', headers={'X-Yinghui-Token': token})
                with urllib.request.urlopen(request, timeout=2) as response: stats = json.load(response)
                break
            except (OSError, urllib.error.URLError): time.sleep(.3)
        else: raise TimeoutError('Private engine startup timed out')
        try:
            urllib.request.urlopen(f'http://127.0.0.1:{port}/system_stats', timeout=2)
            raise RuntimeError('Unauthenticated request was accepted')
        except urllib.error.HTTPError as error:
            if error.code != 401: raise
        report.update(status='passed', startup_seconds=round(time.monotonic()-start, 2),
                      unauthenticated_request_rejected=True, devices=stats.get('devices'), system=stats.get('system'))
    except Exception as error:
        report['error'] = str(error)
    finally:
        process.terminate()
        try: process.wait(timeout=20)
        except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=10)
args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
raise SystemExit(0 if report['status'] == 'passed' else 1)
