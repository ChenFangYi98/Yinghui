Compositor is MIT licensed. Original copyright: Wonder Assembly LLC, 2026.

The Windows application dynamically links the unmodified Qt 6.12.0 open source
Widgets, Gui, Core, Concurrent, Svg and image format modules under LGPL v3.
Users may replace compatible Qt DLLs; no integrity check prevents relinking.
Qt sources and build instructions: https://download.qt.io/archive/qt/6.12/6.12.0/
and https://code.qt.io/cgit/qt/. Qt license texts and module SPDX inventories are
included in this directory. Qt source copyright and third party component
notices are recorded in those inventories.

Bundled libraries and upstream sources:

- libtiff 4.7.0: https://download.osgeo.org/libtiff/ (libtiff license).
- Little CMS 2.17: https://github.com/mm2/Little-CMS (MIT).
- miniz 3.0.2: https://github.com/richgel999/miniz (MIT).
- zlib 1.3.1: https://github.com/madler/zlib (zlib license).
- libjpeg-turbo 3.1.2: https://github.com/libjpeg-turbo/libjpeg-turbo (IJG/BSD/zlib).
- Noto Sans SC fallback font: https://github.com/google/fonts/tree/main/ofl/notosanssc
  (SIL Open Font License 1.1; pinned upstream font and license are included).
- Microsoft Visual C++ runtime DLLs and D3DCompiler: Microsoft redistributable
  components, licensed under their Microsoft distribution terms.

Offline vision pack:

- ONNX Runtime 1.20.1 CPU: https://github.com/microsoft/onnxruntime (MIT;
  upstream third party notices included with the dynamically linked runtime).
- MobileSAM encoder and prompt/mask decoder, source revision
  f706ad9c4eb7f219c00d9050e46328518ffb65d2:
  https://github.com/ChaoningZhang/MobileSAM (Apache-2.0). Converted to ONNX;
  PyTorch/ORT numerical outputs are compared by scripts/bootstrap_models.py.
- ViTMatte small composition-1k, revision
  6a58ad7646403c1df626fbd746900aec7361ea1d:
  https://huggingface.co/hustvl/vitmatte-small-composition-1k (Apache-2.0).
  Converted to a fixed 512 x 512 ONNX graph; PyTorch/ORT outputs are compared.
- RTMPose-m body7 256 x 192 (official pre-exported ONNX model, 2023-05-04):
  https://github.com/open-mmlab/mmpose/tree/main/projects/rtmpose (Apache-2.0).
  Graph integrity, CPU load and native sample inference are checked. A fresh
  PyTorch export equivalence check has not been performed for this graph.

This distribution includes the corresponding license files. The complete
Windows implementation and CMake source are supplied in the parent repository;
there are no local code modifications to the third party libraries. The image
CLI launcher described below receives a Windows UTF-8 process manifest.

Embedded generative AI pack:

- Qwen-Image-2.1 INT8 convrot and Qwen3-VL visual text encoder, copied unchanged
  from the user's local ComfyUI installation. Repackaged/quantized by Comfy-Org:
  https://huggingface.co/Comfy-Org/Qwen-Image-2.1 . The Qwen Research License
  permits research/evaluation use only; commercial use requires a separate
  license. Full license and required attribution are in local-qwen/licenses/.
- ComfyUI 0.37.0, GPL-3.0: https://github.com/Comfy-Org/ComfyUI . A private copy
  of the core source is included in local-qwen/comfy, without custom nodes or
  user jobs. Python 3.13, PyTorch and dependency notices remain in the private
  runtime's LICENSE and dist-info files. The separate local_comfy_server.py
  integration adds loopback authentication without changing upstream files.

- Qwen3.8-27B, Apache-2.0: https://huggingface.co/Qwen/Qwen3.8-27B .
  Unsloth UD-Q4_K_M GGUF and F16 vision projector, revision
  4ca720788d1e01f1bff70c033e0d0028fd02e502. Quantized derivative;
  original license is in ai/Qwen3.8-LICENSE.txt. Weight hashes and sizes are
  recorded in ai/manifest.json, with the lightweight model retained separately.
- Qwen3.5-2B, Apache-2.0: https://huggingface.co/Qwen/Qwen3.5-2B .
  Unsloth Q4_K_M GGUF and F16 vision projector, revision
  f6d5376be1edb4d416d56da11e5397a961aca8ae; the original model is quantized.
- llama.cpp b11560, MIT: https://github.com/ggml-org/llama.cpp .
  CPU, Vulkan and CUDA Windows runtime libraries are distributed unchanged.
  LLVM OpenMP notice and NVIDIA CUDA EULA accompany the pack. NVIDIA runtime
  libraries are redistributed under the CUDA Toolkit redistribution terms;
  no display driver or CUDA developer toolkit is installed.
- Stable Diffusion 1.5, CreativeML Open RAIL-M:
  https://huggingface.co/stable-diffusion-v1-5/stable-diffusion-v1-5 .
  Second State Q4_0 GGUF, revision 031b5f5df991f511b3f5fa8fed6d99048ababb69;
  the original model is quantized. The original license and its use restrictions
  apply to the model. No claim is made that generated images are copyright-free.
- stable-diffusion.cpp master-956-1b0ba10, MIT:
  https://github.com/leejet/stable-diffusion.cpp . CPU and Vulkan binaries are
  libraries are distributed unchanged, with their dynamic dependencies. The
  sd-cli launcher receives a UTF-8 activeCodePage resource so Chinese installation
  and temporary paths work on Windows. No inference algorithm code is modified;
  scripts/bootstrap_embedded_image.py contains the reproducible resource change.
