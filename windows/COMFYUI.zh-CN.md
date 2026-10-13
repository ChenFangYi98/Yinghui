# 用中文想法运行 ComfyUI

本次已检测本机 `http://127.0.0.1:8188`，并用现有千问图片编辑模型执行了软件内局部修图、文字生图和两图参考任务。原文件均保留。这个脚本不调用云端，也不需要千问 API 密钥。

## 在 Compositor 中使用

1. 启动 ComfyUI，确保浏览器可以打开它。
2. 打开“AI 连接中心”，选择“ComfyUI（本机）”，地址填 `http://127.0.0.1:8188`。
3. 点“获取模型 / 测试连接”，在“账户可用模型”输入 `qwen_image_edit` 搜索并选择模型，再点“保存配置”。本机已安装 `qwen_image_edit_2511_fp8mixed.safetensors`。
4. 在 AI 区域选择 ComfyUI 为图片引擎。软件自动构造千问图片工作流，并寻找本机 `qwen_2.5_vl_7b` 编码器和 `qwen_image_vae`。
5. 打开图片，说出想法。只想改一块时先圈出区域；生成结果在本机按原始蒙版合成，选区外保留。

模型列表同时显示 `checkpoints` 和 `diffusion_models`。连接检测不要求先选择模型，也不生成图片。
内置模板支持普通 Stable Diffusion 检查点、Qwen Image Edit 2509/2511 家族。Flux、视频模型以及其他架构需要匹配的自定义 API 工作流；不能仅靠改文件名变成另一种架构。

连接中心可以拖动边缘放大、最大化、拖动中间分隔条。小窗口会滚动，保存与取消始终在底部；下次打开记住窗口大小。

## 更简单的独立脚本

先安装 Python 3.10 或更新版本。图片输入需要 Pillow，在 PowerShell 运行一次：

```powershell
python -m pip install Pillow
```

**把一张或最多三张图片拖到 `ComfyUI-中文修图.cmd` 上**，然后输入中文想法。
直接双击这个文件则只输入文字生图。两图、三图时可以说“图1是人物，图2是背景，保持人物衣服不变”。
多张参考图会一起交给千问图片模型；`--batch` 则逐张处理。

模型设置在 `workflows/comfyui/settings.example.json`，默认已填写本机现有模型。
同一家族只需修改 `model` 文件名；更换编码器或 VAE 时同时修改 `text_encoder` 和 `vae`。
这里填写 ComfyUI 模型列表中的文件名，不是磁盘绝对路径。
普通检查点改为 `"family": "checkpoint"`；千问图片模型使用 `"family": "qwen-image-edit"`。

默认 1024×1024、20 步。宽高需为 16 的倍数；步数、随机种子、等待超时均可改。
如果安装了专门的 Lightning 加速 LoRA，须使用包含该 LoRA 的工作流；直接把普通模型步数改成4不等于加速模型。

## 常用命令

以下命令在安装目录或 `windows` 目录运行，含空格的路径加双引号。

```powershell
# 只检测连接并列出本机模型，不生图
python scripts/comfyui_client.py --check

# 只输入文字
python scripts/comfyui_client.py --prompt "一张简洁的蓝色水彩海报，不要文字"

# 修改一张图
python scripts/comfyui_client.py --prompt "人物保持不变，把背景换成海边" --input "照片.png"

# 两张图一起参考；最多三张
python scripts/comfyui_client.py --prompt "将图1的商品放进图2的场景，保留商品文字" --input "商品.png" "场景.jpg"

# 多张图片分别处理，不融合
python scripts/comfyui_client.py --batch --prompt "自然提亮，保留细节" --input "照片1.jpg" "照片2.tif"

# 白色蒙版区域修改、黑色区域保留；蒙版尺寸与图1一致
python scripts/comfyui_client.py --prompt "去掉选中的杂物，延续周围背景" --input "照片.png" --mask "区域.png"

# 基于上一次结果继续；填写上次输出的实际文件名
python scripts/comfyui_client.py --prompt "背景再暖一点" --input "comfyui-output/上次结果.png"

# 导出并验证工作流：会上传指定输入，但不提交生图
python scripts/comfyui_client.py --prompt "换背景" --input "照片.png" --dry-run "我的工作流.json"
```

输出默认在 `comfyui-output`，使用不同文件名保存每次结果，原文件不覆盖；可用 `--output "D:/我的结果"` 改目录。
PNG、JPEG、WebP、TIFF 等图片由 Pillow 解码为8位 RGBA，旋转方向会展开，上传不带原 EXIF。
生成质量、文字准确度及人物一致性取决于模型；脚本不会保证所有想法都能成功。

千问模板的蒙版用于**本机最终合成保护**，并非给模型增加原生局部重绘能力。需要专用重绘模型时使用对应工作流。
普通检查点模板会同时把蒙版传给 `VAEEncodeForInpaint`；最终仍在本机合成，支持透明像素替换。

## 高级：自己的 API 工作流

在 ComfyUI 开启开发者选项，导出 **API 格式** JSON。节点应含 `class_type`、`inputs`；界面的 `nodes/links` JSON 不能直接运行。
脚本通过 `--workflow "工作流.json"` 加载；配置增加 `bindings`，例如：

```json
{
  "prompt": "8.prompt",
  "model": "1.unet_name",
  "image1": "20.image",
  "image2": "21.image",
  "width": "6.width",
  "height": "6.height",
  "seed": "10.seed",
  "output": "7"
}
```

映射的数字是你工作流的节点 ID，必须根据实际工作流填写。只映射实际使用的输入；缺图、缺节点或模型不可用会在排队前提示。`output` 可以是一个或多个 `SaveImage` 节点 ID。
同一工作流也可通过 Compositor 连接中心的“ComfyUI 工作流…”导入；软件内当前为单文档图片，使用 `image` 映射，而脚本支持 `image1`、`image2`、`image3`。

取消脚本只停止等待，不调用全局 `/interrupt`，避免影响其他 ComfyUI 任务。超时也不会自动再次生图；可在 ComfyUI 页面查看已提交任务。

协议和模型依据：[ComfyUI 服务接口](https://docs.comfy.org/development/comfyui-server/comms_routes)、[千问图片编辑官方模板说明](https://docs.comfy.org/tutorials/image/qwen/qwen-image-edit-2511)。
