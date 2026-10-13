@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo 拖一张或多张图片到本文件上，可按中文描述修图；直接双击则文字生图。
echo 模型设置在 workflows\comfyui\settings.example.json。
python scripts\comfyui_client.py --input %*
pause
