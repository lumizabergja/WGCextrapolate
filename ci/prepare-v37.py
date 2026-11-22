from pathlib import Path

p=Path("ci/build-appveyor.ps1")
s=p.read_text(encoding="utf-8-sig")
marker="Apply-Patch 'ui-v35.patch' 'Simplified UI with crosshair radius and feather'"
assert s.count(marker)==1, "Unsupported build pipeline"
s=s.replace(marker,marker+"""
Write-Host 'Apply V37 V35-based center and border protections...'
& python 'ci/apply-v37-guard.py'
if ($LASTEXITCODE -ne 0) { throw 'V37 guard source patch failed' }
$updatedMain = Get-Content 'source/main.cpp' -Raw
if ($updatedMain -notmatch 'IDC_BORDER_PERCENT' -or
    $updatedMain -notmatch 'borderFraction' -or
    $updatedMain -notmatch 'Num32BitValues = 6') { throw 'V37 guarded source validation failed' }
""",1)

marker="Write-Host 'Compiling with MSVC x64...'"
assert s.count(marker)==1
s=s.replace(marker,"""# Validate both stages of the new runtime-compiled prediction shader with DXC.
$sourceText = Get-Content 'source/main.cpp' -Raw
$start = 'static const char shader[] = R"HLSL('
$stop = ')HLSL";'
$a = $sourceText.IndexOf($start)
if($a -lt 0){ throw 'V37 embedded HLSL start missing' }
$a += $start.Length
$b = $sourceText.IndexOf($stop,$a)
if($b -le $a){ throw 'V37 embedded HLSL end missing' }
[IO.File]::WriteAllText('source/v37-guard.hlsl',$sourceText.Substring($a,$b-$a),$utf8)
& $dxc.FullName /nologo /T vs_6_0 /E VS /O3 /Fo source/v37-guard-vs.cso source/v37-guard.hlsl
if($LASTEXITCODE -ne 0){throw 'V37 vertex shader compile failed'}
& $dxc.FullName /nologo /T ps_6_0 /E PS /O3 /Fo source/v37-guard-ps.cso source/v37-guard.hlsl
if($LASTEXITCODE -ne 0){throw 'V37 pixel shader compile failed'}

"""+marker,1)
assert "FG-Extrap-V35.exe" in s and "FG-V35-Extrapolation-Reflex-Windows-x64.zip" in s
s=s.replace("FG-Extrap-V35.exe","FG-Extrap-V37.exe")
s=s.replace("FG-V35-Extrapolation-Reflex-Windows-x64.zip","FG-V37-V35-StableCenterBorders-Windows-x64.zip")
s=s.replace("V35 DXGI/WGC ->","V37 V35-base DXGI/WGC ->")
s=s.replace(" / radius / feather'"," / hard-center radius / outer feather / four-edge guard percent'")
s=s.replace("'Stop and game process exit: keep app open',",
            "'Stop and game process exit: keep app open (requires actual Windows runtime test)',")
assert "v37-guard-ps.cso" in s and "FG-Extrap-V37.exe" in s
p.write_text(s,encoding="utf-8")
print("Prepared CI for strict V35-based V37 guard build")
