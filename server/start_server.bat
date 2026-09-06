@echo off
chcp 65001 >nul
title esp32talking server
cd /d %~dp0

echo ================================================
echo  esp32talking 服务器已启动
echo  本机测试:   http://localhost:8000
echo  手机/ESP32: ws://本机IP:8000/ws  (本机IP用 ipconfig 查看)
echo  停止: Ctrl+C 或直接关闭本窗口
echo ================================================

.venv\Scripts\python.exe server.py

pause
