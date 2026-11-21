@echo off
setlocal EnableExtensions
cd /d "%~dp0"
call Build.cmd
if errorlevel 1 exit /b 1
if not exist "bin\FG-WGC.exe" (
  echo ERROR: Compiled EXE missing.
  exit /b 1
)
if not exist "artifacts" mkdir "artifacts"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Compress-Archive -Path 'bin/FG-WGC.exe','README.md' -DestinationPath 'artifacts/FG-WGC-D3D11-LowLatency-Windows-x64.zip' -Force; if (!(Test-Path 'artifacts/FG-WGC-D3D11-LowLatency-Windows-x64.zip')) { throw 'Pack failed' }"
if errorlevel 1 exit /b 1
exit /b 0
