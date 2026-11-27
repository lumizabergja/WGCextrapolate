$ErrorActionPreference = 'Stop'
$env:NGX_SDK = (Resolve-Path ngx-sdk).Path
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (!$vs) { throw 'MSVC missing' }
$vcvars = Join-Path $vs.Trim() 'VC\Auxiliary\Build\vcvars64.bat'
& cmd.exe /d /s /c ('call "' + $vcvars + '" && call fg\CI-Build.cmd > build.log 2>&1')
$status = $LASTEXITCODE
if (Test-Path build.log) { Get-Content build.log }
if ($status -ne 0) { throw "Compile or tests failed: $status" }
if (!(Test-Path fg/bin/FG.exe)) { throw 'FG.exe missing' }
New-Item -ItemType Directory -Force artifacts | Out-Null
Compress-Archive -LiteralPath fg\bin\FG.exe,fg\bin\fg.vert.spv,fg\bin\fg.frag.spv,fg\README.md -DestinationPath artifacts\FG-Live-Native-NGX-Windows-x64.zip -Force
