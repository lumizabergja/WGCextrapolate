$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force 'vendor/headers' | Out-Null
$urls=@{
  'nvFBC.h'='https://raw.githubusercontent.com/joelngwt/CloudyNvCapture/master/inc/NvFBC/nvFBC.h'
  'NvFBCToDx9Vid.h'='https://raw.githubusercontent.com/joelngwt/CloudyNvCapture/master/inc/NvFBC/nvFBCToDx9Vid.h'
  'nvOpticalFlowD3D11.h'='https://raw.githubusercontent.com/SlimRG/NvidiaOF5/main/nvOpticalFlowD3D11.h'
  'nvOpticalFlowCommon.h'='https://raw.githubusercontent.com/SlimRG/NvidiaOF5/main/nvOpticalFlowCommon.h'
}
foreach($name in $urls.Keys) {
  Invoke-WebRequest -UseBasicParsing -Uri $urls[$name] -OutFile "vendor/headers/$name"
  if((Get-Item "vendor/headers/$name").Length -lt 1024) { throw "Invalid SDK header: $name" }
}
