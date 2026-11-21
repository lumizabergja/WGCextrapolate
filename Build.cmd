@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if not exist "src\main.cpp" (echo ERROR: Missing source; run python prepare_source.py & exit /b 2)
if not exist bin mkdir bin
cl /nologo /std:c++20 /utf-8 /O2 /MT /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 src\main.cpp /Fe:bin\FG-LG-StartStop.exe /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib
if errorlevel 1 (echo ERROR: Start/Stop DXGI compile failed & exit /b 1)
if not exist "bin\FG-LG-StartStop.exe" (echo ERROR: Missing EXE & exit /b 2)
exit /b 0
