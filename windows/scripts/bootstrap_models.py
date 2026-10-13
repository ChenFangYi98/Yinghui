"""Build and validate the optional offline vision pack from upstream assets.

Run with .tools/model-export/Scripts/python.exe after installing the packages
listed in model-export-requirements.txt. Export never uploads user images.
"""
from pathlib import Path
import hashlib
import json
import shutil
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / '.tools'
PACK = TOOLS / 'vision-models'
DOWNLOADS = TOOLS / 'downloads'
SAM_COMMIT = 'f706ad9c4eb7f219c00d9050e46328518ffb65d2'

def fetch(url, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        temporary = target.with_suffix(target.suffix + '.partial')
        print('Download:', target.name, flush=True)
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        temporary.replace(target)
    return target

def extract_zip(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        for name in bundle.namelist():
            path = (destination / name).resolve()
            if not path.is_relative_to(destination.resolve()):
                raise ValueError('Unsafe archive member')
        bundle.extractall(destination)

def main():
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    torch.set_num_threads(8)
    PACK.mkdir(parents=True, exist_ok=True)
    sdk = fetch('https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip',
                DOWNLOADS / 'onnxruntime-win-x64-1.20.1.zip')
    if not (TOOLS / 'onnxruntime/onnxruntime-win-x64-1.20.1/include/onnxruntime_cxx_api.h').exists():
        extract_zip(sdk, TOOLS / 'onnxruntime')
    pose = fetch('https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/'
                 'rtmpose-m_simcc-body7_pt-body7_420e-256x192-e48f03d0_20230504.zip', DOWNLOADS / 'rtmpose-m.zip')
    extract_zip(pose, TOOLS / 'rtmpose-source')
    shutil.copy2(next((TOOLS / 'rtmpose-source').rglob('end2end.onnx')), PACK / 'rtmpose-m.onnx')
    source = fetch('https://codeload.github.com/ChaoningZhang/MobileSAM/tar.gz/' + SAM_COMMIT,
                   DOWNLOADS / ('MobileSAM-' + SAM_COMMIT + '.tar.gz'))
    source_dir = TOOLS / ('MobileSAM-' + SAM_COMMIT)
    if not source_dir.exists():
        with tarfile.open(source) as bundle:
            if bundle.pax_headers.get('comment') != SAM_COMMIT:
                raise ValueError('MobileSAM source revision mismatch')
            bundle.extractall(TOOLS, filter='data')
    sys.path.insert(0, str(source_dir))
    from mobile_sam import sam_model_registry
    from mobile_sam.utils.onnx import SamOnnxModel
    sam = sam_model_registry['vit_t'](checkpoint=str(source_dir / 'weights/mobile_sam.pt')).eval()
    image = torch.randn(1, 3, 1024, 1024)
    encoder = PACK / 'mobilesam-encoder.onnx'
    if not encoder.exists():
        print('Export MobileSAM image encoder', flush=True)
        with torch.no_grad():
            torch.onnx.export(sam.image_encoder, image, str(encoder), input_names=['image'],
                              output_names=['image_embeddings'], opset_version=17, dynamo=False)
    with torch.no_grad():
        expected = sam.image_encoder(image).numpy()
    actual = ort.InferenceSession(str(encoder), providers=['CPUExecutionProvider']).run(None, {'image': image.numpy()})[0]
    np.testing.assert_allclose(actual, expected, rtol=5e-3, atol=5e-3)
    decoder = SamOnnxModel(sam, return_single_mask=True, use_stability_score=True).eval()
    inputs = (torch.from_numpy(expected), torch.tensor([[[100., 100.], [700., 900.]]]),
              torch.tensor([[2., 3.]]), torch.zeros(1, 1, 256, 256), torch.zeros(1), torch.tensor([1024., 1024.]))
    names = ['image_embeddings', 'point_coords', 'point_labels', 'mask_input', 'has_mask_input', 'orig_im_size']
    decoder_path = PACK / 'mobilesam-decoder.onnx'
    if not decoder_path.exists():
        print('Export MobileSAM prompt encoder and mask decoder', flush=True)
        with torch.no_grad():
            torch.onnx.export(decoder, inputs, str(decoder_path), input_names=names,
                output_names=['masks', 'iou_predictions', 'low_res_masks'], opset_version=17, dynamo=False,
                dynamic_axes={'point_coords': {1: 'points'}, 'point_labels': {1: 'points'}})
    with torch.no_grad():
        expected_masks = decoder(*inputs)
    outputs = ort.InferenceSession(str(decoder_path), providers=['CPUExecutionProvider']).run(None,
                {name: value.numpy() for name, value in zip(names, inputs)})
    for actual, expected in zip(outputs, expected_masks):
        np.testing.assert_allclose(actual, expected.numpy(), rtol=1e-2, atol=1e-2)
    from transformers import VitMatteForImageMatting
    matte = VitMatteForImageMatting.from_pretrained('hustvl/vitmatte-small-composition-1k',
        revision='6a58ad7646403c1df626fbd746900aec7361ea1d', use_safetensors=True).eval()
    class Matte(torch.nn.Module):
        def __init__(self, model): super().__init__(); self.model = model
        def forward(self, image): return self.model(pixel_values=image).alphas
    wrapper = Matte(matte).eval()
    matte_input = torch.rand(1, 4, 512, 512)
    matte_path = PACK / 'vitmatte-small.onnx'
    if not matte_path.exists():
        print('Export ViTMatte', flush=True)
        with torch.no_grad():
            torch.onnx.export(wrapper, matte_input, str(matte_path), input_names=['image'], output_names=['alpha'],
                              opset_version=17, dynamo=False)
    with torch.no_grad(): expected = wrapper(matte_input).numpy()
    actual = ort.InferenceSession(str(matte_path), providers=['CPUExecutionProvider']).run(None, {'image': matte_input.numpy()})[0]
    np.testing.assert_allclose(actual, expected, rtol=1e-2, atol=1e-2)
    report = {'runtime': 'onnxruntime 1.20.1 CPU', 'validated': True,
              'source_revisions': {'MobileSAM': SAM_COMMIT,
                  'ViTMatte': '6a58ad7646403c1df626fbd746900aec7361ea1d'}, 'models': {}}
    for model in PACK.glob('*.onnx'):
        onnx.checker.check_model(str(model))
        session = ort.InferenceSession(str(model), providers=['CPUExecutionProvider'])
        report['models'][model.name] = {'sha256': hashlib.sha256(model.read_bytes()).hexdigest(),
            'bytes': model.stat().st_size, 'inputs': {v.name: v.shape for v in session.get_inputs()},
            'outputs': {v.name: v.shape for v in session.get_outputs()},
            'checks': ['onnx-checker', 'CPU-session-load'] + (
                ['PyTorch-ORT-numerical-comparison'] if model.name != 'rtmpose-m.onnx' else ['official-preexported-graph'])}
    for name, url in [('MobileSAM-LICENSE.txt', 'https://raw.githubusercontent.com/ChaoningZhang/MobileSAM/master/LICENSE'),
                      ('ViTMatte-LICENSE.txt', 'https://raw.githubusercontent.com/hustvl/ViTMatte/main/LICENSE'),
                      ('MMPose-LICENSE.txt', 'https://raw.githubusercontent.com/open-mmlab/mmpose/main/LICENSE')]:
        fetch(url, PACK / name)
    (PACK / 'validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    for name, url in [('truck.jpg', 'https://raw.githubusercontent.com/facebookresearch/segment-anything/main/notebooks/images/truck.jpg'),
                      ('person.jpg', 'https://raw.githubusercontent.com/open-mmlab/mmpose/main/tests/data/coco/000000000785.jpg')]:
        fetch(url, TOOLS / 'vision-samples' / name)
    print('Vision pack validated:', PACK, flush=True)

if __name__ == '__main__': main()
