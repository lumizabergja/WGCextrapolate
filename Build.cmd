@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if not exist "%CUDA_PATH%\include\cuda.h" (
    echo Set CUDA_PATH to the NVIDIA CUDA Toolkit installation.
    exit /b 2
)
where cl.exe >nul 2>nul
if errorlevel 1 (
    echo Open an x64 Visual Studio Developer Command Prompt before building.
    exit /b 2
)
rem Always rebuild PTX: host and kernel SourcePair ABI must match.
"%CUDA_PATH%\bin\nvcc.exe" -std=c++14 -O3 -ptx -arch=compute_75 "src\predict.cu" -o "src\predict.ptx"
if errorlevel 1 exit /b 1
set "RCFLAGS="
if /i "%~1"=="--blackwell" (
    "%CUDA_PATH%\bin\nvcc.exe" -std=c++14 -O3 -cubin -arch=sm_120 "src\predict.cu" -o "src\predict.cubin"
    if errorlevel 1 exit /b 1
    set "RCFLAGS=/d FG_BLACKWELL"
)
if not exist bin mkdir bin
powershell -NoProfile -ExecutionPolicy Bypass -File Fetch-NVAPI.ps1
if errorlevel 1 exit /b 1
rc /nologo %RCFLAGS% /i "src" /fo "bin\FG.res" "src\FG.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /utf-8 /O2 /MD /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /I "vendor\headers" /I "%CUDA_PATH%\include" /I nvapi-sdk "src\main.cpp" nvapi-sdk\nvapi64.lib "bin\FG.res" /Fo"bin\main.obj" /Fe"bin\FG.exe" /link /SUBSYSTEM:WINDOWS bcrypt.lib wintrust.lib crypt32.lib version.lib shlwapi.lib shell32.lib advapi32.lib ole32.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib dwmapi.lib uxtheme.lib
exit /b %errorlevel%
