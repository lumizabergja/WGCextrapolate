$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force 'include' | Out-Null
# SDK 5.0 public interface headers. These are pulled as a matched pair, not mixed with SDK 2.0.
$base='https://raw.githubusercontent.com/SlimRG/NvidiaOF5/master/'
foreach($name in @('nvOpticalFlowCommon.h','nvOpticalFlowD3D11.h')) {
  Invoke-WebRequest -Uri ($base + $name) -OutFile (Join-Path 'include' $name) -UseBasicParsing
  if ((Get-Item (Join-Path 'include' $name)).Length -lt 3000) {throw "Invalid SDK header: $name"}
}
Write-Host 'Retrieved matching NVIDIA Optical Flow SDK 5.0 API headers.'
