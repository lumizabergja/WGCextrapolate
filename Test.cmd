@echo off
setlocal
cd /d "%~dp0"
if not exist bin mkdir bin
for %%T in (display_clock_tests flow_handoff pixel_tests) do (
    cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 "tests\%%T.cpp" /Fo"bin\%%T.obj" /Fe"bin\%%T.exe"
    if errorlevel 1 exit /b 1
    "bin\%%T.exe"
    if errorlevel 1 exit /b 1
)
