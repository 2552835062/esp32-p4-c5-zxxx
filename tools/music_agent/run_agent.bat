@echo off
title Music Agent
chcp 65001 >nul
cd /d %~dp0
echo Starting NetEase music agent...
echo.
python -u music_agent.py
echo.
echo Agent exited. Press any key to close this window.
pause >nul
