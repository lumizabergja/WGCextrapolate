$ErrorActionPreference = 'Stop'
$ref = '9981c6350dc58c0ce21f024fd65e31e851c31689'
$base = "https://raw.githubusercontent.com/NVIDIA/DLSS/$ref"
New-Item -ItemType Directory -Force probe-sdk, bin | Out-Null
foreach ($h in @('nvsdk_ngx.h','nvsdk_ngx_defs.h','nvsdk_ngx_params.h','nvsdk_ngx_loader.h')) {
 Invoke-WebRequest "$base/include/$h" -OutFile "probe-sdk/$h"
}
Invoke-WebRequest "$base/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" -OutFile "probe-sdk/nvsdk_ngx_d.lib"
if ((Get-Item 'probe-sdk/nvsdk_ngx_d.lib').Length -lt 1024) { throw 'NGX SDK library download incomplete' }
