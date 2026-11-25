@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File Fetch-Probe-SDK.ps1
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MD /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I probe-sdk src\ngx11_probe.cpp probe-sdk\nvsdk_ngx_d.lib /Fe:bin\FG-NGX-DX11-Probe.exe /link d3d11.lib dxgi.lib wintrust.lib crypt32.lib version.lib shlwapi.lib shell32.lib advapi32.lib ole32.lib bcrypt.lib
if errorlevel 1 exit /b 1
