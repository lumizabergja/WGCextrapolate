$ErrorActionPreference = 'Stop'
$base='https://raw.githubusercontent.com/NVIDIA/TensorRT/b8db91e15be2cae4465ac17fab19e0f969e45407/include'
New-Item -ItemType Directory -Force trt-sdk | Out-Null
foreach($h in @('NvInfer.h','NvInferImpl.h','NvInferLegacyDims.h','NvInferPlugin.h','NvInferPluginBase.h','NvInferPluginUtils.h','NvInferRuntime.h','NvInferRuntimeBase.h','NvInferRuntimeCommon.h','NvInferRuntimePlugin.h','NvInferVersion.h','NvOnnxConfig.h')){
    Invoke-WebRequest "$base/$h" -OutFile "trt-sdk/$h"
}
Get-Content trt-sdk/NvInferVersion.h | Select-String 'NV_TENSORRT_(MAJOR|MINOR|PATCH|BUILD)'
