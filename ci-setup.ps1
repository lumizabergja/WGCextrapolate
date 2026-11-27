$ErrorActionPreference = 'Stop'
python prepare.py
if ($LASTEXITCODE -ne 0) { throw 'Baseline reconstruction failed' }
python unpack-live.py
if ($LASTEXITCODE -ne 0) { throw 'Live patch failed' }
git init ngx-sdk
if ($LASTEXITCODE -ne 0) { throw 'NGX init failed' }
git -C ngx-sdk remote add origin https://github.com/NVIDIA/DLSS.git
if ($LASTEXITCODE -ne 0) { throw 'NGX origin failed' }
git -C ngx-sdk fetch --depth 1 origin 9981c6350dc58c0ce21f024fd65e31e851c31689
if ($LASTEXITCODE -ne 0) { throw 'NGX SDK fetch failed' }
git -C ngx-sdk checkout --detach FETCH_HEAD
if ($LASTEXITCODE -ne 0) { throw 'NGX SDK checkout failed' }
$lib = Get-Item ngx-sdk\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib
if ($lib.Length -lt 1024) { throw 'NGX SDK library missing or LFS pointer only' }
& .\fg\Install-SDKs.ps1
