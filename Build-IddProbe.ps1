$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (!$vs) { throw 'Visual Studio MSBuild not found' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (!(Test-Path $msbuild)) { throw 'x64 MSBuild not found' }
& "$PSScriptRoot\Prepare-IddProbe.ps1"
$cpp = 'upstream/video/IndirectDisplay/IddSampleDriver/Driver.cpp'
$source = [IO.File]::ReadAllText((Resolve-Path $cpp))
$source = $source.Replace('AvRevertMmThreadCharacteristics(AvTaskHandle);','if (AvTaskHandle) AvRevertMmThreadCharacteristics(AvTaskHandle);')
$source = $source.Replace('CoCreateGuid(&MonitorInfo.MonitorContainerId);','if (FAILED(CoCreateGuid(&MonitorInfo.MonitorContainerId))) return;')
$source = $source.Replace('Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);','Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut); if (!NT_SUCCESS(Status)) { WdfObjectDelete(MonitorCreateOut.MonitorObject); return; }')
[IO.File]::WriteAllText((Resolve-Path $cpp), $source)
& $msbuild upstream/video/IndirectDisplay/IddSampleDriver.sln /m /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:RunCodeAnalysis=false /p:EnablePREfast=false /p:CodeAnalysisTreatWarningsAsErrors=false /p:TreatWarningAsError=false /p:SignMode=Off
if ($LASTEXITCODE) { throw "Driver build failed: $LASTEXITCODE" }
$base = 'upstream/video/IndirectDisplay'
$dll = Get-ChildItem $base -Recurse -Filter IddSampleDriver.dll | Select-Object -First 1
$app = Get-ChildItem $base -Recurse -Filter IddSampleApp.exe | Select-Object -First 1
$inf = Get-ChildItem $base -Recurse -Filter IddSampleDriver.inf | Where-Object { $_.FullName -match 'Release|x64' } | Select-Object -First 1
if (!$dll -or !$app -or !$inf) { throw 'Missing build outputs' }
New-Item -ItemType Directory -Force package/driver | Out-Null
Copy-Item $dll.FullName,$inf.FullName package/driver/
Copy-Item $app.FullName package/
Copy-Item README.md,Enable-Report.ps1 package/
Copy-Item upstream/LICENSE package/Microsoft-Sample-LICENSE.txt
Get-ChildItem package -Recurse
Compress-Archive -Path 'package/*' -DestinationPath 'FG-320-IddCx-Probe-x64-UNSIGNED.zip' -Force
