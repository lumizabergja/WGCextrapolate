@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if not exist "src\main.cpp" (echo ERROR: Missing verified source & exit /b 2)
if not exist "vendor\headers\nvOpticalFlowD3D11.h" (echo ERROR: Missing NVIDIA optical flow header & exit /b 2)
if not exist bin mkdir bin
rc /nologo /i "src" /fo "bin\FG.res" "src\FG.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /utf-8 /O2 /MT /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /I "vendor\headers" src\main.cpp "bin\FG.res" /Fe:bin\FG-LG-DXGI-FreezeFix.exe /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib dwmapi.lib uxtheme.lib
if errorlevel 1 (echo ERROR: Looking Glass DXGI MSVC compile failed & exit /b 1)
if not exist bin\FG-LG-DXGI-FreezeFix.exe exit /b 1
exit /b 0
