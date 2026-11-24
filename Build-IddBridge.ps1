$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
if (!$vs) { throw 'MSVC not found' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (!(Test-Path $msbuild)) { throw 'x64 MSBuild not found' }
& "$PSScriptRoot\Prepare-IddProbe.ps1"
$dir = 'upstream/video/IndirectDisplay'
$cpp = "$dir/IddSampleDriver/Driver.cpp"
$source = [IO.File]::ReadAllText((Resolve-Path $cpp))
function Replace-Required($s,$a,$b) {
    if (!$s.Contains($a)) { throw "Missing source anchor: $a" }
    return $s.Replace($a,$b)
}
$source = Replace-Required $source '#include <cstdio>' ('#include <cstdio>' + [Environment]::NewLine + '#include "FGBridge.h"')
$source = Replace-Required $source 'AvRevertMmThreadCharacteristics(AvTaskHandle);' 'if (AvTaskHandle) AvRevertMmThreadCharacteristics(AvTaskHandle);'
$source = Replace-Required $source 'CoCreateGuid(&MonitorInfo.MonitorContainerId);' 'if (FAILED(CoCreateGuid(&MonitorInfo.MonitorContainerId))) return;'
$source = Replace-Required $source 'Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);' 'Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut); if (!NT_SUCCESS(Status)) { WdfObjectDelete(MonitorCreateOut.MonitorObject); return; }'
$source = Replace-Required $source 'unsigned fgFrames=0, fgTextures=0;' 'FGBridge fgBridge;'
$source = Replace-Required $source 'ULONGLONG fgStart=GetTickCount64();' ''
$start = $source.IndexOf('            ++fgFrames;')
$end = $source.IndexOf('            // ==============================', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Diagnostic block missing' }
$source = $source.Substring(0,$start) + '            fgBridge.Send(m_Device->Device.Get(),m_Device->DeviceContext.Get(),AcquiredBuffer.Get());' + [Environment]::NewLine + $source.Substring($end)
[IO.File]::WriteAllText((Resolve-Path $cpp),$source)
Copy-Item FGBridge.h "$dir/IddSampleDriver/"
$infSrc="$dir/IddSampleDriver/IddSampleDriver.inf"
$s=[IO.File]::ReadAllText((Resolve-Path $infSrc)).Replace('FGIddProbe','FGIddBridge').Replace('FG IddCx Probe','FG IddCx Bridge')
[IO.File]::WriteAllText((Resolve-Path $infSrc),$s)
$appSrc="$dir/IddSampleApp/main.cpp"
$s=[IO.File]::ReadAllText((Resolve-Path $appSrc)).Replace('FGIddProbe','FGIddBridge').Replace('FG IddCx Probe','FG IddCx Bridge')
[IO.File]::WriteAllText((Resolve-Path $appSrc),$s)
& $msbuild "$dir/IddSampleDriver.sln" /m /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:RunCodeAnalysis=false /p:EnablePREfast=false /p:CodeAnalysisTreatWarningsAsErrors=false /p:TreatWarningAsError=false /p:SignMode=Off
if ($LASTEXITCODE) { throw "Driver build failed: $LASTEXITCODE" }
$dev = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$line = 'call "' + $dev + '" && call Build.cmd'
& cmd.exe /d /c $line
if ($LASTEXITCODE -or !(Test-Path bin/FG.exe)) { throw 'FG app build failed' }
$dll = Get-ChildItem $dir -Recurse -Filter IddSampleDriver.dll | Select-Object -First 1
$app = Get-ChildItem $dir -Recurse -Filter IddSampleApp.exe | Select-Object -First 1
$inf = Get-ChildItem $dir -Recurse -Filter IddSampleDriver.inf | Where-Object { $_.FullName -match 'Release|x64' } | Select-Object -First 1
if (!$dll -or !$app -or !$inf) { throw 'Missing driver build outputs' }
New-Item -ItemType Directory -Force package/driver | Out-Null
Copy-Item $dll.FullName,$inf.FullName package/driver/
Copy-Item $app.FullName package/
Copy-Item bin/FG.exe,README.md,Install-Driver.ps1 package/
Copy-Item upstream/LICENSE package/Microsoft-Sample-LICENSE.txt
# Create test-signed package if the SDK tools are available; never install a certificate or change boot settings.
$kit=Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Windows Kits\10\bin\10.0.26100.0'
$sig=Join-Path $kit 'x64\signtool.exe'
$catTool=Join-Path $kit 'x86\Inf2Cat.exe'
if (!(Test-Path $catTool)) {$catTool=Join-Path $kit 'x64\Inf2Cat.exe'}
$status='UNSIGNED: no production driver signature.'
if ((Test-Path $sig) -and (Test-Path $catTool)) {
    $cert=$null
    try {
        $cert=New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=FG IddCx Development Test Only' -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable -HashAlgorithm SHA256
        & $sig sign /fd SHA256 /sha1 $cert.Thumbprint /s My package/driver/IddSampleDriver.dll
        if ($LASTEXITCODE) {throw 'DLL test signing failed'}
        & $catTool "/driver:$(Resolve-Path package/driver)" /os:10_VB_X64,10_CO_X64,10_NI_X64,10_GE_X64 /uselocaltime
        if ($LASTEXITCODE) {throw 'Inf2Cat verification failed'}
        & $sig sign /fd SHA256 /sha1 $cert.Thumbprint /s My package/driver/IddSampleDriver.cat
        if ($LASTEXITCODE) {throw 'Catalog test signing failed'}
        Export-Certificate -Cert $cert -FilePath package/FG-Development-Test.cer | Out-Null
        $status='TEST-SIGNED ONLY: for an authorized test environment. Not Microsoft-signed. No security settings or certificate stores changed on the target PC.'
    } catch {
        Write-Warning $_
        $status='UNSIGNED/INCOMPLETE TEST SIGNATURE: driver requires proper signing.'
        Remove-Item package/driver/*.cat,package/FG-Development-Test.cer -ErrorAction SilentlyContinue
    } finally {
        if ($cert) {Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue}
    }
}
$status | Set-Content package/Signing-Status.txt
Get-ChildItem package -Recurse
Compress-Archive -Path 'package/*' -DestinationPath 'FG-320-IddCx-Integrated-x64.zip' -Force
