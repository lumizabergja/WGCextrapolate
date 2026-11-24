$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$cat=Join-Path $PSScriptRoot 'driver\IddSampleDriver.cat'
if (!(Test-Path $cat)) {throw 'No signed catalog. Obtain a correctly signed package before installation.'}
$s=Get-AuthenticodeSignature -LiteralPath $cat
if ($s.Status -ne 'Valid') {throw "Catalog is not trusted ($($s.Status)). Do not change security settings to bypass this."}
& pnputil.exe /add-driver "$PSScriptRoot\driver\IddSampleDriver.inf" /install
if ($LASTEXITCODE) {throw "Driver staging failed: $LASTEXITCODE"}
Write-Host 'Package staged. Run IddSampleApp.exe to create the virtual display.'
