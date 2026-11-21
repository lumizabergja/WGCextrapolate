$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force 'vendor/headers' | Out-Null
$urls=@{
  'nvOpticalFlowD3D11.h'='https://raw.githubusercontent.com/SlimRG/NvidiaOF5/main/nvOpticalFlowD3D11.h'
  'nvOpticalFlowCommon.h'='https://raw.githubusercontent.com/SlimRG/NvidiaOF5/main/nvOpticalFlowCommon.h'
}
foreach($name in $urls.Keys){
  Invoke-WebRequest -UseBasicParsing -Uri $urls[$name] -OutFile (Join-Path 'vendor/headers' $name)
  if((Get-Item (Join-Path 'vendor/headers' $name)).Length -lt 1024){throw "Failed downloading $name"}
}
