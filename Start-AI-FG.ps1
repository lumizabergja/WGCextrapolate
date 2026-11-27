$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$dll=(Get-Content -LiteralPath 'AI/trt-dll.txt' -Raw).Trim()
if(!(Test-Path $dll)){throw 'TensorRT runtime missing; run Setup-AI-Depth.ps1 again'}
if(!(Test-Path 'AI/zipdepth_512x384.plan')){throw 'Depth engine missing'}
$env:FG_TRT_DLL=$dll
$env:PATH="$(Split-Path $dll);$env:PATH"
if(!(Test-Path 'FG.exe')){throw 'Put this script beside FG.exe'}
& .\FG.exe
