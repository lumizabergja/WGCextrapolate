# AppVeyor Windows x64 build: V35 custom extrapolation + Streamline Reflex only
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$utf8 = [Text.UTF8Encoding]::new($false)
$nl = [string][char]10
function Normalize-Patch([string]$inputText) {
  return $inputText.Replace(([string][char]13 + [string][char]10), $nl)
}
function Apply-Patch([string]$path, [string]$label) {
  & git apply --check $path
  if ($LASTEXITCODE -ne 0) { throw "$label patch check failed" }
  & git apply $path
  if ($LASTEXITCODE -ne 0) { throw "$label patch apply failed" }
}

Write-Host 'Reconstructing original V35 source...'
$parts = @(Get-ChildItem 'v26build/source_part_*.b64' | Sort-Object Name)
if ($parts.Count -eq 0) { throw 'V26 source archive parts are missing' }
$b64 = ($parts | ForEach-Object { (Get-Content $_.FullName -Raw).Trim() }) -join ''
[IO.File]::WriteAllBytes('v26_source.tar.xz', [Convert]::FromBase64String($b64))
$expected = 'cdb3f9d4cbb3231be04e07ed6089707eeae9d9c3e747d89593263f13177a6829'
if ((Get-FileHash v26_source.tar.xz -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
  throw 'Base archive SHA256 mismatch'
}
& tar -xf v26_source.tar.xz
if ($LASTEXITCODE -ne 0) { throw 'V26 source extraction failed' }

$v27 = Normalize-Patch ((@('v27patch/part_0.patch','v27patch/part_1.patch','v27patch/part_2.patch') | ForEach-Object { Get-Content $_ -Raw }) -join '')
[IO.File]::WriteAllText('v27-current.patch', $v27, $utf8)
Apply-Patch 'v27-current.patch' 'V27'
Apply-Patch 'v28patch/flush1-copy.patch' 'V28'
Apply-Patch 'v29patch/release-after-publish.patch' 'V29'
[IO.File]::WriteAllText('v30-current.patch',(Normalize-Patch (Get-Content 'v30patch/split-alttab.patch' -Raw)),$utf8)
Apply-Patch 'v30-current.patch' 'V30'
[IO.File]::WriteAllBytes('v31-current.patch', [Convert]::FromBase64String((Get-Content 'v31patch/alt-tab-window.b64' -Raw).Trim()))
Apply-Patch 'v31-current.patch' 'V31'
[IO.File]::WriteAllText('v32-current.patch',(Normalize-Patch (Get-Content 'v32patch/fg6x-grid1x1-8x8.patch' -Raw)),$utf8)
Apply-Patch 'v32-current.patch' 'V32'
[IO.File]::WriteAllText('v33-current.patch',(Normalize-Patch (Get-Content 'v33patch/best-overall.patch' -Raw)),$utf8)
Apply-Patch 'v33-current.patch' 'V33'
foreach ($item in @('v34patch/01-dlss.patch','v34patch/02-main.patch','v34patch/03-nvof.patch')) {
  [IO.File]::WriteAllText('v34-current.patch',(Normalize-Patch (Get-Content $item -Raw)),$utf8)
  Apply-Patch 'v34-current.patch' $item
}
$v35 = (Get-ChildItem 'v35patch/part_*.patch' | Sort-Object Name | ForEach-Object { Get-Content $_.FullName -Raw }) -join ''
[IO.File]::WriteAllText('v35-current.patch',(Normalize-Patch $v35),$utf8)
Apply-Patch 'v35-current.patch' 'V35'
Apply-Patch 'extrap-v35.patch' 'Custom extrapolation'
Apply-Patch 'ui-v35.patch' 'Simplified UI with crosshair radius and feather'
Apply-Patch 'premium-v35.patch' 'Premium GDI+ interface and persistent application window'
# Apply the V15 Clean V16 center-mask and lifecycle port to the reconstructed source.
$encoded=(Get-Content 'ci/v15-crosshair-update.py.gz.b64' -Raw).Trim()
[IO.File]::WriteAllBytes('ci/v15-crosshair-update.py.gz',[Convert]::FromBase64String($encoded))
& python -c "import gzip,pathlib;pathlib.Path('ci/v15-crosshair-update.py').write_bytes(gzip.decompress(pathlib.Path('ci/v15-crosshair-update.py.gz').read_bytes()))"
if ($LASTEXITCODE -ne 0) { throw 'V15 source-update unpack failed' }
& python 'ci/v15-crosshair-update.py'
if ($LASTEXITCODE -ne 0) { throw 'V15 center-mask/native-only/Stop patch failed' }


# GDI+ imaging interfaces inherit IUnknown, which must be declared before
# gdiplus.h is parsed. Include objidl.h explicitly on the Windows SDK runner.
$premiumHeader = Get-Content 'source/premium_ui.h' -Raw
$premiumHeader = $premiumHeader.Replace('#include <gdiplus.h>', '#include <objidl.h>' + $nl + '#include <gdiplus.h>')
[IO.File]::WriteAllText('source/premium_ui.h', $premiumHeader, $utf8)


$main = Get-Content 'source/main.cpp' -Raw
$bridge = Get-Content 'source/dlss_bridge.cpp' -Raw
if ($bridge -match 'kFeatureDLSS_G' -or $bridge -match 'slDLSSGSetOptions') {
  throw 'DLSS-G feature code is still present after applying the extrapolation patch'
}
if ($bridge -notmatch 'kFeatureReflex' -or $main -notmatch 'Extrap') {
  throw 'Reflex or extrapolation code was not found in the patched source'
}
if ($main -notmatch 'IDC_CROSSHAIR_RADIUS' -or $main -notmatch 'IDC_CROSSHAIR_FEATHER' -or
    $main -notmatch 'dynamicTarget_' -or $main -notmatch 'protectFeather') {
  throw 'Simplified UI or DX12 crosshair protection patch missing'
}
if ($main -notmatch 'DrawPremiumUI' -or $main -notmatch 'RequestStop' -or
    $main -match 'gDLSS.EndRuntime\(\)' -or $main -notmatch 'PremiumHit') {
  throw 'Premium UI or non-closing capture Stop lifecycle is missing'
}
if (-not (Test-Path 'source/premium_ui.h')) { throw 'Premium themed controls header missing' }
if ($main -notmatch 'workerFinished_' -or $main -notmatch 'FinalizeFinishedSession' -or
    $main -notmatch 'float centerMask' -or $main -notmatch 'float\(d.width\),float\(d.height\)') {
  throw 'V15 center-mask/persistent Stop lifecycle validation failed'
}
if ($main -match 'ofaPercent_' -or $main -match 'CB_GETCURSEL,0,0\).*IDC_NVOF_RESOLUTION') {
  throw 'OFA resolution scaling/UI selector was not fully removed'
}
if ($main -notmatch 'SetMaximumFrameLatency\(1\)') {
  throw 'V35 low-latency swapchain setting is missing'
}

Write-Host 'Retrieving pinned NVIDIA SDK dependencies...'
& git clone --depth 1 https://github.com/mbucchia/Optical-Flow-SDK.git OpticalFlow
if ($LASTEXITCODE -ne 0) { throw 'Optical Flow SDK clone failed' }
$ofaInclude = Join-Path $root 'OpticalFlow\NvOFInterface'
if (-not (Test-Path $ofaInclude)) { throw 'NvOFInterface headers missing from SDK checkout' }

$release = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip'
& curl.exe --fail --location --retry 3 --output StreamlineSDK.zip $release
if ($LASTEXITCODE -ne 0) { throw 'NVIDIA Streamline 2.14.1 SDK download failed' }
Expand-Archive -LiteralPath 'StreamlineSDK.zip' -DestinationPath 'StreamlineSDK' -Force
$headers = @(Get-ChildItem -Path 'StreamlineSDK' -File -Recurse -Filter 'sl.h' | Where-Object { $_.DirectoryName -match '[\\/]include$' })
if ($headers.Count -eq 0) { throw 'Streamline sl.h not found in SDK archive' }
$slInclude = $headers[0].DirectoryName
foreach ($header in @('sl_reflex.h','sl_pcl.h')) {
  if (-not (Test-Path (Join-Path $slInclude $header))) { throw "Streamline header missing: $header" }
}

Write-Host 'Regenerating SM6 bytecode...'
$headerText = Get-Content 'source/motion_shader.h' -Raw
$startMarker = 'R"HLSL('
$endMarker = ')HLSL"'
$start = $headerText.IndexOf($startMarker)
$end = $headerText.LastIndexOf($endMarker)
if ($start -lt 0 -or $end -le $start) { throw 'HLSL markers not found in motion_shader.h' }
$hlsl = $headerText.Substring($start + $startMarker.Length, $end - $start - $startMarker.Length)
[IO.File]::WriteAllText('source/motion_shader.hlsl',$hlsl,$utf8)
$pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
$dxc = Get-ChildItem (Join-Path $pf86 'Windows Kits\10\bin') -File -Recurse -Filter 'dxc.exe' |
  Where-Object { $_.FullName -match '[\\/]x64[\\/]dxc.exe$' } |
  Sort-Object FullName -Descending | Select-Object -First 1
if (-not $dxc) { throw 'Windows SDK dxc.exe is not installed' }
& $dxc.FullName /nologo /T cs_6_0 /E Resample /O3 /Fo source\resample.cso source\motion_shader.hlsl
if ($LASTEXITCODE -ne 0) { throw 'Resample shader compile failed' }
& $dxc.FullName /nologo /T cs_6_0 /E Motion /O3 /Fo source\motion.cso source\motion_shader.hlsl
if ($LASTEXITCODE -ne 0) { throw 'Motion shader compile failed' }
function To-CArray([byte[]]$bytes) { return ($bytes | ForEach-Object { [string]$_ }) -join ',' }
$r = To-CArray([IO.File]::ReadAllBytes((Resolve-Path 'source/resample.cso').Path))
$m = To-CArray([IO.File]::ReadAllBytes((Resolve-Path 'source/motion.cso').Path))
$bytecode = '#pragma once' + $nl + 'static const unsigned char resampleCode[]={' + $r + '};' +
  $nl + 'static const unsigned char motionCode[]={' + $m + '};' + $nl
[IO.File]::WriteAllText('source/compute_bytecode.h',$bytecode,$utf8)

Write-Host 'Compiling with MSVC x64...'
$vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe was not found' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools were not found' }
$vcvars = Join-Path ($vs | Select-Object -First 1) 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw 'vcvars64.bat was not found' }
$batch = @"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
cd /d "$root\source"
rc /nologo /fo app.res app.rc
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /std:c++20 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /I "$slInclude" /I "$ofaInclude" main.cpp dlss_bridge.cpp nvof_bridge.cpp wgc_capture.cpp app.res /Fe:"$root\FG-Extrap-V35.exe" /link /SUBSYSTEM:WINDOWS d3d12.lib d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib comctl32.lib shell32.lib ole32.lib user32.lib gdi32.lib wintrust.lib crypt32.lib windowsapp.lib
if errorlevel 1 exit /b 1
"@
[IO.File]::WriteAllText('compile-v35-appveyor.cmd', $batch, [Text.Encoding]::ASCII)
Write-Host 'MSVC batch setup:'
Get-Content 'compile-v35-appveyor.cmd'
& cmd.exe /d /c 'call compile-v35-appveyor.cmd'
if ($LASTEXITCODE -ne 0 -or -not (Test-Path 'FG-Extrap-V35.exe')) {
  throw 'MSVC compile failed, so no ready-to-run release can be published'
}

Write-Host 'Packaging Reflex-only runtime and executable...'
New-Item -ItemType Directory -Path 'artifacts\payload' -Force | Out-Null
Copy-Item -LiteralPath 'FG-Extrap-V35.exe' -Destination 'artifacts\payload\FG-Extrap-V35.exe'
foreach ($name in @('sl.interposer.dll','sl.common.dll','sl.pcl.dll','sl.reflex.dll')) {
  $candidates = @(Get-ChildItem 'StreamlineSDK' -File -Recurse -Filter $name |
    Where-Object { $_.FullName -match '[\\/]bin[\\/]x64[\\/]' -and $_.FullName -notmatch '[\\/]development[\\/]' })
  if ($candidates.Count -eq 0) { throw "Required signed Reflex runtime missing from NVIDIA SDK: $name" }
  Copy-Item -LiteralPath $candidates[0].FullName -Destination (Join-Path 'artifacts\payload' $name)
}
$sha = (Get-FileHash 'FG-Extrap-V35.exe' -Algorithm SHA256).Hash.ToLowerInvariant()
$validation = @(
  'V35 DXGI/WGC -> NvOFA -> custom extrapolation (Off-6x and Dynamic) -> DX12',
  'UI: game selector + Refresh / capture / extrapolation / dynamic target / radius / feather; Native 4x4 NvOFA only',
  'Stop behavior: completed-worker handoff modeled on V15 Clean V16; game exit preserves UI',
  'Streamline Reflex + PCL ONLY; no DLSS-G plugin shipped',
  "Git commit: $env:APPVEYOR_REPO_COMMIT",
  "Executable SHA256: $sha",
  'Build verification: MSVC compiler returned success; no end-to-end GPU/game runtime test performed.'
)
[IO.File]::WriteAllLines('artifacts\payload\BUILD-VALIDATION.txt',$validation)
Compress-Archive -Path 'artifacts\payload\*' -DestinationPath 'artifacts\FG-V35-Premium-UI-Fixed-Windows-x64.zip' -Force
if (-not (Test-Path 'artifacts\FG-V35-Premium-UI-Fixed-Windows-x64.zip')) {
  throw 'AppVeyor ZIP artifact was not produced'
}
Write-Host 'Build and packaging succeeded.'
