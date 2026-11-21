$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force 'vendor/headers' | Out-Null
$urls=@{
 'NvFBCApi.h'='https://raw.githubusercontent.com/garrettsparks/nvfbc-relay/d2cb213370f1c1963cbd53d2704fa2355aa961c0/src/common/NvFBCApi.h'
 'nvOpticalFlowD3D11.h'='https://raw.githubusercontent.com/SlimRG/NvidiaOF5/main/nvOpticalFlowD3D11.h'
 'nvOpticalFlowCommon.h'='https://raw.githubusercontent.com/SlimRG/NvidiaOF5/main/nvOpticalFlowCommon.h'
}
foreach($name in $urls.Keys){
 Invoke-WebRequest -UseBasicParsing -Uri $urls[$name] -OutFile "vendor/headers/$name"
 if((Get-Item "vendor/headers/$name").Length -lt 1024){throw "Downloaded header invalid: $name"}
}
