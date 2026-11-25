@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if not exist src\main.cpp exit /b 2
if not exist src\FG.ico exit /b 2
if not exist vendor\headers\nvOpticalFlowD3D11.h exit /b 2
if not exist bin mkdir bin
powershell -NoProfile -ExecutionPolicy Bypass -File Fetch-Probe-SDK.ps1
if errorlevel 1 exit /b 1
rc /nologo /i "src" /fo "bin\FG.res" "src\FG.rc"
if errorlevel 1 exit /b 1
if not defined FG_OFA_SLOT_COUNT set "FG_OFA_SLOT_COUNT=2"
cl /nologo /std:c++20 /utf-8 /O2 /MD /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DFG_OFA_SLOT_COUNT=%FG_OFA_SLOT_COUNT% /D_WIN32_WINNT=0x0A00 /I "vendor\headers" /I probe-sdk src\main.cpp probe-sdk\nvsdk_ngx_d.lib "bin\FG.res" /Fe:bin\FG.exe /link /SUBSYSTEM:WINDOWS bcrypt.lib wintrust.lib crypt32.lib version.lib shlwapi.lib shell32.lib advapi32.lib ole32.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib dwmapi.lib uxtheme.lib comctl32.lib windowsapp.lib runtimeobject.lib
if errorlevel 1 exit /b 1
if exist FrameWarpRuntime xcopy /E /I /Y FrameWarpRuntime bin\FrameWarpRuntime >nul
exit /b 0
