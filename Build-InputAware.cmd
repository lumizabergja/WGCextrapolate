@echo off
setlocal EnableExtensions
cd /d "%~dp0"
where cl >nul 2>&1
if not errorlevel 1 goto build
set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo Install Visual Studio C++ desktop build tools and a recent Windows SDK.
    pause
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
:build
call Build.cmd
if errorlevel 1 (
    echo Build failed. See output above.
    pause
    exit /b 1
)
echo Build succeeded: bin\FG.exe
pause
