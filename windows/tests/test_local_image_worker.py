import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/local_image_worker.py'
MODULE = importlib.util.spec_from_file_location('local_image_worker', SCRIPT)
worker = importlib.util.module_from_spec(MODULE)
MODULE.loader.exec_module(worker)


class LocalImageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.spec = {'id': 'qwen-image-21', 'repo': 'Qwen/Test', 'name': 'Fixture', 'revision': 'fixed',
                     'license': 'research', 'generate': True, 'files': []}
        self.content = b'original verified weights'
        for name in ['weights.safetensors', 'model_index.json']:
            path = self.root / 'models/qwen-image-21' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.content)
            self.spec['files'].append({'path': name, 'bytes': len(self.content),
                                      'sha256': hashlib.sha256(self.content).hexdigest()})

    def test_installed_files_revalidate_when_changed(self):
        worker.verify_model(self.root, self.spec)
        path = self.root / 'models/qwen-image-21/weights.safetensors'
        path.write_bytes(b'x' * len(self.content))
        with self.assertRaisesRegex(RuntimeError, 'checksum'):
            worker.verify_model(self.root, self.spec)

    def test_incomplete_file_is_not_ready(self):
        (self.root / 'models/qwen-image-21/weights.safetensors').write_bytes(b'partial')
        with self.assertRaisesRegex(RuntimeError, 'incomplete'):
            worker.verify_model(self.root, self.spec)
        self.assertFalse((self.root / 'models/qwen-image-21/verified.json').exists())

    def test_revision_change_invalidates_cached_verification(self):
        worker.verify_model(self.root, self.spec)
        changed = copy.deepcopy(self.spec)
        changed['revision'] = 'different'
        with patch.object(worker, 'digest', wraps=worker.digest) as digest:
            worker.verify_model(self.root, changed)
            self.assertEqual(digest.call_count, 2)

    def test_research_license_checked_before_downloading(self):
        with self.assertRaisesRegex(ValueError, 'license'):
            worker.download_model(self.root, self.spec, False)

    def test_corrupt_complete_file_downloads_again(self):
        path = self.root / 'models/qwen-image-21/weights.safetensors'
        path.write_bytes(b'x' * len(self.content))
        downloads = []
        def download(**kwargs):
            downloads.append(kwargs)
            (kwargs['local_dir'] / kwargs['filename']).write_bytes(self.content)
        with patch.dict(sys.modules, {'huggingface_hub': types.SimpleNamespace(hf_hub_download=download)}):
            worker.download_model(self.root, self.spec, True)
        self.assertEqual(len(downloads), 1)
        self.assertTrue(downloads[0]['force_download'])
        self.assertFalse(downloads[0]['token'])
        self.assertEqual(downloads[0]['revision'], 'fixed')

    def test_unknown_and_traversal_models_rejected(self):
        path = self.root / 'catalog.json'
        path.write_text(json.dumps({'version': 1, 'models': [self.spec]}))
        with self.assertRaisesRegex(ValueError, 'Unknown'):
            worker.model_spec(path, '../other')
        self.spec['files'][0]['path'] = '../outside'
        path.write_text(json.dumps({'version': 1, 'models': [self.spec]}))
        with self.assertRaisesRegex(ValueError, 'path'):
            worker.model_spec(path, 'qwen-image-21')

    def test_edit_only_model_does_not_claim_generation(self):
        self.spec['generate'] = False
        with self.assertRaisesRegex(ValueError, 'edits existing'):
            worker.request_options({'prompt': 'test'}, self.spec)

    def test_resolution_preserves_aspect_and_never_upscales_unnecessarily(self):
        options = worker.request_options({'prompt': 'test', 'width': 1920, 'height': 1080}, self.spec)
        self.assertEqual(options[2], (1024, 576))
        small = worker.request_options({'prompt': 'test', 'width': 256, 'height': 128}, self.spec)
        self.assertEqual(small[2], (256, 128))

    def test_bad_parameters_fail_before_importing_inference(self):
        for update in [{'steps': 0}, {'steps': 81}, {'device': 'unknown'}, {'maxEdge': 999},
                       {'prompt': ''}, {'width': -1}, {'operation': 'shell'}]:
            with self.subTest(update=update), self.assertRaises(ValueError):
                worker.request_options({'prompt': 'test', **update}, self.spec)

    def test_offline_runtime_never_inherits_token_or_endpoint(self):
        with patch.dict(os.environ, {'HF_TOKEN': 'private', 'HF_ENDPOINT': 'https://other', 'PYTHONPATH': 'other'}):
            worker.configure(self.root, offline=True)
            self.assertNotIn('HF_TOKEN', os.environ)
            self.assertNotIn('HF_ENDPOINT', os.environ)
            self.assertNotIn('PYTHONPATH', os.environ)
            self.assertEqual(os.environ['HF_HUB_OFFLINE'], '1')

    def test_quantized_workflow_uses_qwen21_cache_and_reference_slots(self):
        manifest = {'modelFile': 'model.safetensors', 'encoderFile': 'encoder.safetensors', 'vaeFile': 'vae.safetensors'}
        graph = worker.comfy_graph(manifest, 'test', ['source.png', 'selection.png'], (768, 512), 25, 7)
        self.assertEqual(graph['5']['inputs']['images.image_1'], ['21', 0])
        self.assertEqual(graph['5']['inputs']['images.image_2'], ['22', 0])
        self.assertEqual(graph['5']['inputs']['resolution'], 0)
        self.assertEqual(graph['4']['class_type'], 'QwenImage21Cache')
        self.assertEqual(graph['7']['inputs']['cfg'], 1.0)
        self.assertEqual(graph['7']['inputs']['steps'], 25)
        self.assertNotIn('ModelSamplingAuraFlow', [n['class_type'] for n in graph.values()])

    def test_generation_latent_uses_requested_aspect_ratio(self):
        manifest = dict(modelFile='m', encoderFile='e', vaeFile='v')
        graph = worker.comfy_graph(manifest, 'test', [], (768, 512), 25, 1)
        self.assertEqual(graph['6']['inputs']['width'], 384)
        self.assertEqual(graph['6']['inputs']['height'], 256)
        self.assertNotIn('images.image_1', graph['5']['inputs'])

    def test_private_job_cleanup_cannot_delete_other_files(self):
        base = self.root / 'comfy-jobs/input'
        base.mkdir(parents=True)
        owned = base / 'Yinghui_abc.png'
        original = base / 'photo.png'
        outside = self.root / 'Yinghui_outside.png'
        for path in (owned, original, outside): path.write_bytes(b'fixture')
        worker.cleanup_comfy(self.root, [('input', {'name': owned.name}),
            ('input', {'name': original.name}),
            ('input', {'name': '../../Yinghui_outside.png'}),
            ('input', {'name': outside.name, 'subfolder': '../..'})])
        self.assertFalse(owned.exists())
        self.assertTrue(original.exists())
        self.assertTrue(outside.exists())


if __name__ == '__main__':
    unittest.main()
