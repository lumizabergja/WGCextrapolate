$ErrorActionPreference = 'Stop'
function Invoke-CheckedGit {
    param([Parameter(Mandatory=$true)][string]$Step, [Parameter(Mandatory=$true)][string[]]$GitArgs)
    # Run Git through cmd so PowerShell never interprets Git stderr as ErrorRecords.
    $quoted = @($GitArgs | ForEach-Object { '"' + $_ + '"' })
    $command = 'git.exe ' + ($quoted -join ' ') + ' 2>&1'
    & cmd.exe /d /c $command
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "$Step failed (git exit $code)" }
}
python prepare.py
if ($LASTEXITCODE -ne 0) { throw 'Baseline reconstruction failed' }
python unpack-live.py
if ($LASTEXITCODE -ne 0) { throw 'Live patch failed' }
python reflex-late-input.py
if ($LASTEXITCODE -ne 0) { throw 'Adaptive Reflex 2 patch failed' }
python native-output.py
if ($LASTEXITCODE -ne 0) { throw 'Strict native output patch failed' }
Invoke-CheckedGit -Step 'NGX init' -GitArgs @('init','ngx-sdk')
Invoke-CheckedGit -Step 'NGX origin' -GitArgs @('-C','ngx-sdk','remote','add','origin','https://github.com/NVIDIA/DLSS.git')
Invoke-CheckedGit -Step 'NGX SDK fetch' -GitArgs @('-C','ngx-sdk','fetch','--depth','1','origin','9981c6350dc58c0ce21f024fd65e31e851c31689')
Invoke-CheckedGit -Step 'NGX SDK checkout' -GitArgs @('-C','ngx-sdk','checkout','--detach','FETCH_HEAD')
$rev = & git.exe -C ngx-sdk rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $rev.Trim() -ne '9981c6350dc58c0ce21f024fd65e31e851c31689') { throw 'NGX SDK revision mismatch' }
$lib = Get-Item ngx-sdk\lib\Windows_x86_64\x64\nvsdk_ngx_d.lib
if ($lib.Length -lt 1024) { throw 'NGX SDK library missing or LFS pointer only' }
& .\fg\Install-SDKs.ps1
