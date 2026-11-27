$ErrorActionPreference='Stop'
$base='https://raw.githubusercontent.com/NVIDIA/nvapi/51e5aae4bc40c3ccd3fb7d4dfd94256e7bfc496e'
New-Item -ItemType Directory -Force nvapi-sdk | Out-Null
foreach($h in @('nvapi.h','nvapi_lite_common.h','nvapi_lite_d3dext.h','nvapi_lite_salend.h','nvapi_lite_salstart.h','nvapi_lite_sli.h','nvapi_lite_stereo.h','nvapi_lite_surround.h')){
  Invoke-WebRequest "$base/$h" -OutFile "nvapi-sdk/$h"
}
Invoke-WebRequest "$base/amd64/nvapi64.lib" -OutFile 'nvapi-sdk/nvapi64.lib'
