@echo off
cd /d "%~dp0"
if not exist FrameWarpRuntime\nvngx_latewarp.dll (
 echo Copy the original FrameWarpRuntime folder next to this executable first.
 pause
 exit /b 1
)
FG-NGX-DX11-Probe.exe FrameWarpRuntime > FG-NGX-DX11-Probe.txt 2>&1
set RC=%errorlevel%
type FG-NGX-DX11-Probe.txt
echo Exit code %RC%
pause
