"""Exercise the shipped vision pack with an isolated PATH and unrelated environment overrides."""
from pathlib import Path
import json
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / 'dist/Yinghui-Windows-0.4.5-x64'


def main():
    contents = json.loads((STAGE / 'PACKAGE-CONTENTS.json').read_text(encoding='utf-8'))
    assert contents['version'] == '0.4.5' and contents['qwen38_27b_offline_pack']
    helper = STAGE / 'compositor_embedded_smoke.exe'
    shutil.copy2(ROOT / 'out/release/compositor_embedded_smoke.exe', helper)
    env = os.environ.copy()
    env['PATH'] = os.environ['SystemRoot'] + r'\System32;' + os.environ['SystemRoot']
    for key in ['QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH']:
        env.pop(key, None)
    env.update({'PYTHONHOME': r'C:\unrelated-python', 'PYTHONPATH': r'C:\unrelated-modules',
                'CUDA_VISIBLE_DEVICES': '-1', 'GGML_BACKEND_PATH': r'C:\unrelated-backends',
                'LLAMA_ARG_MODEL': r'C:\unrelated-model.gguf', 'LLAMA_ARG_PORT': '1',
                'OMP_NUM_THREADS': '1', 'HTTP_PROXY': 'http://127.0.0.1:1',
                'HTTPS_PROXY': 'http://127.0.0.1:1'})
    evidence = ROOT / 'out/staged-qwen38-045.json'
    try:
        subprocess.run([str(helper), '--model', 'qwen38-27b', '--backend', 'auto',
                        '--restart', '--output', str(evidence)], cwd=STAGE, env=env,
                       check=True, timeout=1200)
        result = json.loads(evidence.read_text(encoding='utf-8'))
        assert result['status'] == 'passed' and result['restart_refreshed_connection']
        assert result['model'] == 'Qwen3.8 27B' and 'CUDA' in result['device']
        result['isolated_system_path'] = True
        result['unrelated_environment_overrides'] = True
        evidence.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print('Shipped vision pack passed with isolated PATH and unrelated external overrides')
    finally:
        helper.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
