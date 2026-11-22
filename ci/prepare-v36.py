from pathlib import Path

p = Path("ci/build-appveyor.ps1")
s = p.read_text(encoding="utf-8-sig")

marker = "Write-Host 'Retrieving pinned NVIDIA SDK dependencies...'"
assert s.count(marker) == 1, "Unexpected V35 CI script layout"
insert = r"""
# V36 adaptation: apply the exact user's V35 -> V36 source delta
# after the existing V35 reconstruction/upgrade completes.
Write-Host 'Applying adaptive four-real-frame prediction and persistent Stop fix...'
[IO.File]::WriteAllBytes('v36-upgrade.patch.xz',
  [Convert]::FromBase64String((Get-Content 'v36/upgrade.patch.xz.b64' -Raw).Trim()))
& python -c "import pathlib,lzma; pathlib.Path('v36-upgrade.patch').write_bytes(lzma.decompress(pathlib.Path('v36-upgrade.patch.xz').read_bytes()))"
if ($LASTEXITCODE -ne 0) { throw 'V36 source patch decompression failed' }
& git apply --check 'v36-upgrade.patch'
if ($LASTEXITCODE -ne 0) { throw 'V36 patch does not match reconstructed V35 source: stop rather than compile wrong app' }
& git apply 'v36-upgrade.patch'
if ($LASTEXITCODE -ne 0) { throw 'V36 patch application failed' }
Copy-Item 'v36/adaptive_prediction_shader.h' 'source/adaptive_prediction_shader.h' -Force
$four = Get-Content 'source/main.cpp' -Raw
if ($four -notmatch 'kTemporalFlowSlots = 3' -or $four -notmatch 'RequestStop\\(bool gameClosed\\)' -or
    $four -notmatch 'kAdaptivePredictionHlsl') { throw 'V36 predictor / Stop change was not applied' }
$shader = Get-Content 'source/adaptive_prediction_shader.h' -Raw
if ($shader -notmatch 'validFlowCount >= 3.0') { throw 'Four-frame shader missing' }
"""
s=s.replace(marker,insert+"\n"+marker,1)

# V36 HLSL moved from main.cpp into the dedicated source header.
old="$predictCpp=Get-Content 'source/main.cpp' -Raw"
new="$predictCpp=Get-Content 'source/adaptive_prediction_shader.h' -Raw"
assert old in s
s=s.replace(old,new)
old = """$predictBegin='static const char shader[] = R"HLSL('"""
new = """$predictBegin='R"HLSL('"""
assert old in s
s=s.replace(old,new)

# Name *the compiled executable* V36 rather than repackaging a V35 binary.
s=s.replace("FG-Extrap-V35.exe","FG-Extrap-V36.exe")
s=s.replace("FG-V35-V15Full-StopSafe-Windows-x64.zip","FG-V36-Adaptive4Frame-StopFixed-Windows-x64.zip")
s=s.replace("V35 DXGI/WGC ->", "V36 DXGI/WGC ->")
s=s.replace("V35 custom extrapolation", "V36 adaptive 4-frame extrapolation")

# Verify all script anchors were transformed before allowing a build.
assert "FG-Extrap-V35.exe" not in s
assert "FG-Extrap-V36.exe" in s and "v36-upgrade.patch" in s
assert "$predictCpp=Get-Content 'source/adaptive_prediction_shader.h' -Raw" in s
p.write_text(s, encoding="utf-8")
print("Prepared V36 Windows CI: adaptive motion history, Stop fix, dedicated HLSL")
