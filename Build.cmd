@echo off
setlocal EnableExtensions
cd /d "%~dp0"
if not exist src\main.cpp exit /b 2
if not defined CUDA_PATH (
    echo CUDA Toolkit 12.8 or newer is required for this experimental build.
    exit /b 2
)
if not exist "%CUDA_PATH%\bin\nvcc.exe" exit /b 2
if not exist bin mkdir bin
powershell -NoProfile -ExecutionPolicy Bypass -File Fetch-Probe-SDK.ps1
if errorlevel 1 exit /b 1
rc /nologo /i "src" /fo "bin\FG.res" "src\FG.rc"
if errorlevel 1 exit /b 1
rem RTX 5060 Ti: Blackwell SM 120. Embed native SASS and forward-compatible PTX.
"%CUDA_PATH%\bin\nvcc.exe" -c -O3 -std=c++17 -gencode arch=compute_120,code=sm_120 -gencode arch=compute_120,code=compute_120 -Xcompiler /MD -o bin\dp4a_kernel.obj src\dp4a_kernel.cu
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /utf-8 /O2 /MD /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /I "%CUDA_PATH%\include" /I probe-sdk src\main.cpp bin\dp4a_kernel.obj probe-sdk\nvsdk_ngx_d.lib "bin\FG.res" /Fe:bin\FG.exe /link /SUBSYSTEM:WINDOWS /LIBPATH:"%CUDA_PATH%\lib\x64" cudart_static.lib bcrypt.lib wintrust.lib crypt32.lib version.lib shlwapi.lib shell32.lib advapi32.lib ole32.lib d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib dwmapi.lib uxtheme.lib comctl32.lib windowsapp.lib runtimeobject.lib setupapi.lib
if errorlevel 1 exit /b 1
if exist FrameWarpRuntime xcopy /E /I /Y FrameWarpRuntime bin\FrameWarpRuntime >nul
exit /b 0
