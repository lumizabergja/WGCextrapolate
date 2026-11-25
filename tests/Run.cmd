@echo off
setlocal
cd /d "%~dp0.."
if not exist bin mkdir bin
cl /nologo /EHsc /std:c++20 /O2 /W4 tests\mouse_history.cpp /Fo:bin\mouse_history.obj /Fe:bin\mouse_history.exe
if errorlevel 1 exit /b 1
bin\mouse_history.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++20 /O2 /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX tests\mouse_shader.cpp /Fo:bin\mouse_shader.obj /Fe:bin\mouse_shader.exe /link d3d11.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
bin\mouse_shader.exe
exit /b %errorlevel%
