# Run in elevated PowerShell on a test system, before enabling the virtual display.
$ErrorActionPreference = 'Stop'
$dir = 'C:\ProgramData\FGIddProbe'
if (!(Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$file = Join-Path $dir 'stats.txt'
if (!(Test-Path $file)) { New-Item -ItemType File -Path $file | Out-Null }
# Give the UMDF LocalService account append/write access to this report file only.
icacls $file /grant '*S-1-5-19:(W)'
if ($LASTEXITCODE) { throw 'Could not grant report file access' }
Write-Host "Report path: $file"
