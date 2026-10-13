# Windows 0.4.2 验证记录

日期：2026-10-12。Windows 11 25H2 x64、Intel Core i9-14900K、NVIDIA RTX A6000、Qt 6.12.0、MSVC 2022 Release。项目保持 v15 / 8 位 sRGB。本版范围见 [实施记录](IMPLEMENTATION-0.4.2.zh-CN.md)，用法见 [内置 AI 指南](LOCAL-AI.zh-CN.md)。旧功能验收和边界见 [0.4.1](VALIDATION-0.4.1.md)、[0.4.0](VALIDATION-0.4.0.md)。

## 自动测试

CTest 五套全部通过：111 个测试，不含 10 次初始化 / 清理。核心 22、升级与协议 21、Agent 36、UI 24、内置推理 8。日志：`out/tests-042.log`、`out/release/*-tests.txt`、JUnit XML。

新增覆盖 GPU / CPU 后端顺序、模型包完整性和路径检查、临时凭据不导出、图像请求校验和提前取消、本地预览大小与原图坐标、默认模型分别选择并持久保存、日志独立窗口及实时更新。原有区域、版本、撤销、损坏文件、锁定、协议模拟和格式测试保留。

独立 ComfyUI 脚本 9 项全部通过：`out/comfyui-script-tests-042.log`。独立 TIFF / PSD / PSB 验证：`out/formats-042.log`，合成与图层导入 / 导出的误差检查通过。

## 内置模型真实验证

完整权重已下载并按固定 SHA-256 校验，模型与运行库包含在完整交付包内，使用不依赖外部 Ollama、ComfyUI、Python 或 CUDA Toolkit。模型清单：`.tools/embedded-ai/manifest.json` 与 `.tools/embedded-image/manifest.json`。

- 千问 Qwen3.5 2B Q4_K_M + F16 视觉编码器：中文短回复、红色测试图识别、真实 deselect 工具调用通过。
- CUDA 和强制 CPU 分别完成选区亮度增加 5 的 Agent 流程：一次工具操作、选区外不变、持久版本保存通过。
- 宽泛的自然提亮指令，通过相关工具执行一次局部调色；未执行选区扩大等无关工具。早期发现的重复调用、曝光单位错误和不必要选区工具问题已在本版修正；失败结果没有提交。
- 内置 SD 1.5 Q4：中文描述由文字助手翻译，Vulkan 和强制 CPU 生图、图生图 / 蒙版重绘通过。256×256 蒙版测试改变 16384 个区域内像素，区域外保持原样。
- 中文助手调用内置图片模型生成新图层的完整衔接通过。真实图像检查使用合成测试图片，没有上传用户照片。

证据：`out/embedded-auto-042.json`、`out/embedded-cpu-042.json`、`out/embedded-image-cpu-042.json`、`out/embedded-complete-042.json`、`out/embedded-handoff-042.json`、`out/embedded-final-042.json`。其中 final 是最终 GPU / 自然调色 / 图片 / 衔接流程；早期失败运行不作为已完成验收。Vulkan 文字推理可运行但本机速度明显慢于 CUDA，故 NVIDIA 自动优先 CUDA。CPU 路径实际可运行。

短回复约 0.1 秒 CUDA、约 0.5 秒 CPU；合成选区 Agent 约 1–2 秒 CUDA、约 20 秒 CPU。首次启动包括完整文件校验和加载，约 20–30 秒。以上是这台电脑的短任务测试，不是任意照片、任意电脑的速度保证。图片 4 步烟雾测试只检查管线，不能代表默认 16 步的画面质量；轻量模型不保证复杂图像编辑或准确中文生图文字。

## 界面和常规性能

原生截图覆盖首页、简单模式 100% / 125% / 150%，模型中心 100% / 125% / 150%，本地设备页 100% / 150%。截图与尺寸断言通过，见 `out/ui-validation/results.json`。小窗口 AI 不可用时通过标签提供本机快捷工具，避免把窗口撑大；模型设置与日志分开显示。

额外检查中文安装路径时发现上游图片 CLI 使用系统代码页解析参数，已通过 Windows UTF-8 进程清单修复。中文路径下真实生图成功；完整助手与图片流程见 `out/embedded-unicode-042.json`。这个修改只涉及启动器的进程清单，最终二进制校验值已更新。

4000×4000 八层合成 831 ms，800 px 软笔刷绘制 1000 px 轨迹 661 ms，十次 1024 像素调整预览 619 ms。记录见 `out/benchmark-042.json`；同机结果受并行推理 / 磁盘任务影响，不替代完整设备矩阵性能测试。

## 打包与边界

打包脚本校验每个模型资产，复制 CPU / CUDA / Vulkan 运行库与各目录所需 MSVC DLL；运行时只使用软件目录和 Windows 系统 PATH。发布脚本执行应用启动、原有离线视觉推理，以及完整内置文字 / 图片 CPU 流程；日志见 `out/package-042.log`、`out/embedded-staged-042.json`。安装 / 启动 / 卸载由 `scripts/install_smoke.py` 使用独立测试 AppId 执行，结果见 `out/install-042.log`，不会卸载用户现有正式软件。

只有上述脚本成功并生成 artifact，才完成本次交付；最终文件和校验结果见 `out/delivery-042.json`、`dist/SHA256SUMS.txt`。安装包和便携 ZIP 均包含模型，不是首次运行再下载的空壳。

本轮没有重新调用云端服务；千问云端 / ComfyUI 的上一轮真实证据保留。Windows 10、干净虚拟机、其他 AMD / Intel / NVIDIA 驱动、所有中文输入法及复杂商业图片样本仍未全部实测。Windows CI 配置已更新，本次未远程运行 GitHub Actions。此前未交付的完整 Photoshop 功能仍以旧记录为准。
