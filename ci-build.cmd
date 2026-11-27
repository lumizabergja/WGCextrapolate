@echo off
setlocal EnableExtensions
cd /d "%~dp0"
call ci-env.cmd
if errorlevel 1 exit /b 1
call Build.cmd
if errorlevel 1 exit /b 1
for %%T in (pixel_tests flow_handoff frame_kind) do (
  cl /nologo /std:c++20 /EHsc /O2 tests\%%T.cpp /Fo"bin\%%T.obj" /Fe"bin\%%T.exe"
  if errorlevel 1 exit /b 1
  bin\%%T.exe
  if errorlevel 1 exit /b 1
)
cl /nologo /c /std:c++20 /EHsc /MD /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I "%NGX_SDK%\include" /I "%VULKAN_SDK%\Include" native_ngx\compile_probe.cpp /Fo"bin\latewarp_adapter_probe.obj"
if errorlevel 1 exit /b 1
exit /b 0
