@echo off
setlocal
if not exist bin mkdir bin
if not exist include\nvOpticalFlowD3D11.h (
  echo Missing NVIDIA Optical Flow SDK 5.0 headers. Run Download-Headers.ps1 first.
  exit /b 1
)
cl /nologo /std:c++20 /EHsc /O2 /MD /W3 /DWIN32 /DUNICODE /D_UNICODE /I include src\main.cpp /link /SUBSYSTEM:WINDOWS /OUT:bin\WGC-NvOFA-Fresh.exe
if errorlevel 1 exit /b 1
if not exist bin\WGC-NvOFA-Fresh.exe exit /b 1
