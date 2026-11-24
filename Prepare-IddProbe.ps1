param([string]$Root = "$PSScriptRoot\upstream")
$ErrorActionPreference = 'Stop'
$rev = 'd49bb7d72f3651952b2b6e4ad28d14fe2247eb69'
if (!(Test-Path "$Root\.git")) {
    git clone --filter=blob:none --no-checkout https://github.com/microsoft/Windows-driver-samples.git $Root
    if ($LASTEXITCODE) { throw 'Sample checkout failed' }
}
git -C $Root checkout --detach $rev
if ($LASTEXITCODE) { throw 'Pinned source checkout failed' }
$dir = Join-Path $Root 'video\IndirectDisplay'
function Replace-Exact([string]$Text,[string]$Old,[string]$New) {
    if (!$Text.Contains($Old)) { throw "Patch anchor missing: $Old" }
    return $Text.Replace($Old,$New)
}
$p = Join-Path $dir 'IddSampleDriver\Driver.cpp'
$s = [IO.File]::ReadAllText($p)
$s = Replace-Exact $s '#include "Driver.h"' "#include ""Driver.h""`n#include <cstdio>"
$s = Replace-Exact $s 'IDD_SAMPLE_MONITOR_COUNT = 3;' 'IDD_SAMPLE_MONITOR_COUNT = 1;'
$s = Replace-Exact $s '{ 1920, 1080, 60 },' "{ 1920, 1080, 320 },`n    { 1920, 1080, 240 },`n    { 1920, 1080, 60 },"
$s = Replace-Exact $s 'if (ConnectorIndex >= ARRAYSIZE(s_SampleMonitors))' 'if (true) // FG: one EDID-less virtual monitor'
$s = Replace-Exact $s 'TargetModes.push_back(CreateIddCxTargetMode(1920, 1080, 144));' "TargetModes.push_back(CreateIddCxTargetMode(1920, 1080, 320));`n    TargetModes.push_back(CreateIddCxTargetMode(1920, 1080, 240));`n    TargetModes.push_back(CreateIddCxTargetMode(1920, 1080, 144));"
$s = Replace-Exact $s '    // Acquire and release buffers in a loop' @'
    // FG diagnostic: no pixel readback, no WGC, no desktop duplication.
    ULONGLONG fgStart=GetTickCount64();
    unsigned fgFrames=0, fgTextures=0;
    // Acquire and release buffers in a loop
'@
$s = Replace-Exact $s '            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);' @'
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);
            ++fgFrames;
            ComPtr<ID3D11Texture2D> fgTexture;
            D3D11_TEXTURE2D_DESC fgDesc{};
            if (AcquiredBuffer && SUCCEEDED(AcquiredBuffer.As(&fgTexture))) {
                ++fgTextures;
                fgTexture->GetDesc(&fgDesc);
            }
            const ULONGLONG fgNow=GetTickCount64();
            if (fgNow-fgStart>=1000) {
                char line[256]{};
                int n=std::snprintf(line,sizeof(line),"IDD elapsed_ms=%llu frames=%u gpu_textures=%u width=%u height=%u format=%u adapter_luid=%08x:%08x\r\n",
                    (unsigned long long)(fgNow-fgStart),fgFrames,fgTextures,fgDesc.Width,fgDesc.Height,(unsigned)fgDesc.Format,
                    (unsigned)m_Device->AdapterLuid.HighPart,(unsigned)m_Device->AdapterLuid.LowPart);
                // Only append counters to the pre-created, explicitly enabled report.
                HANDLE f=CreateFileW(L"C:\\ProgramData\\FGIddProbe\\stats.txt",FILE_APPEND_DATA,
                    FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(f!=INVALID_HANDLE_VALUE) {
                    if(n>0 && n<(int)sizeof(line)){DWORD written=0;WriteFile(f,line,(DWORD)n,&written,nullptr);}
                    CloseHandle(f);
                }
                TraceEvents(TRACE_LEVEL_INFORMATION,TRACE_DRIVER,"FG acquired %u buffers, %u GPU textures",fgFrames,fgTextures);
                fgFrames=fgTextures=0;fgStart=fgNow;
            }
'@
[IO.File]::WriteAllText($p,$s)
$p = Join-Path $dir 'IddSampleDriver\IddSampleDriver.inf'
$s = [IO.File]::ReadAllText($p)
$s = $s.Replace('Root\IddSampleDriver','Root\FGIddProbe').Replace('MyDevice_Install, IddSampleDriver','MyDevice_Install, FGIddProbe').Replace('IddSampleDriverGroup','FGIddProbeGroup').Replace('<Your manufacturer name>','FG Experimental').Replace('IddSampleDriver Device','FG IddCx Probe')
# Keep the upstream Windows 11 section. Add a Windows 10 section with its documented reflector service.
$s = Replace-Exact $s 'Standard,NT$ARCH$.10.0...22000' 'Standard,NT$ARCH$.10.0...19041,NT$ARCH$.10.0...22000'
$s = Replace-Exact $s '[Standard.NT$ARCH$.10.0...22000]' @'
[Standard.NT$ARCH$.10.0...19041]
%DeviceName%=MyDevice_Install10, Root\FGIddProbe
%DeviceName%=MyDevice_Install10, FGIddProbe

[Standard.NT$ARCH$.10.0...22000]
'@
$s += @'

[MyDevice_Install10.NT]
CopyFiles=UMDriverCopy

[MyDevice_Install10.NT.hw]
AddReg=MyDevice_HardwareDeviceSettings

[MyDevice_Install10.NT.Services]
AddService=WUDFRd,0x000001fa,WUDFRD_ServiceInstall

[WUDFRD_ServiceInstall]
DisplayName="Windows Driver Foundation - User-mode Driver Framework Reflector"
ServiceType=1
StartType=3
ErrorControl=1
ServiceBinary=%12%\WUDFRd.sys

[MyDevice_Install10.NT.Wdf]
UmdfService=IddSampleDriver,IddSampleDriver_Install
UmdfServiceOrder=IddSampleDriver
UmdfKernelModeClientPolicy=AllowKernelModeClients
'@
[IO.File]::WriteAllText($p,$s)
$p=Join-Path $dir 'IddSampleApp\main.cpp'
$s=[IO.File]::ReadAllText($p).Replace('IddSampleDriver','FGIddProbe').Replace('Idd Sample Driver','FG IddCx Probe')
[IO.File]::WriteAllText($p,$s)
Write-Host "Prepared $dir"
