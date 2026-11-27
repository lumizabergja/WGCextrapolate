param([ValidateSet('320','160','60')][string]$Rate='320')
$ErrorActionPreference='Continue'
Set-Location $PSScriptRoot
$report=Join-Path $PSScriptRoot 'NvPresent-Results.txt'
"FG NvPresent read-only inventory $(Get-Date -Format o)" | Set-Content $report
Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,CurrentRefreshRate | Format-List | Out-String | Add-Content $report
$paths=@("$env:windir\System32\NvPresent64.dll")
$paths+=@(Get-ChildItem "$env:windir\System32\DriverStore\FileRepository" -Directory -Filter 'nv*.inf*' -ErrorAction SilentlyContinue | ForEach-Object {Join-Path $_.FullName 'NvPresent64.dll'})
foreach($p in ($paths | Select-Object -Unique)){
 if(Test-Path -LiteralPath $p){
  $file=Get-Item -LiteralPath $p
  [pscustomobject]@{Path=$p;Version=$file.VersionInfo.FileVersion;Signature=(Get-AuthenticodeSignature -LiteralPath $p).Status;SHA256=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash} | Format-List | Out-String | Add-Content $report
 }
}
foreach($mode in @('dxgi','nvapi')){
 "=== $mode ===" | Tee-Object -FilePath $report -Append
 & "$PSScriptRoot\FG-NvPresent-Probe.exe" $mode $Rate 2>&1 | Tee-Object -FilePath $report -Append
 "Exit code: $LASTEXITCODE" | Tee-Object -FilePath $report -Append
}
Write-Host "Send NvPresent-Results.txt to ChatGPT."
