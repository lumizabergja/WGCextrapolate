@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File Run-Probe.ps1
pause
