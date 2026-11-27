$ErrorActionPreference='Stop'
# Windows PowerShell 5.1 can promote ordinary native stderr progress into
# terminating errors. Only the native process exit code decides success.
function Invoke-Native {
    param([string]$Exe,[string[]]$Arguments)
    $saved=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & $Exe @Arguments 2>&1 | ForEach-Object { Write-Host "$_" }
        $code=$LASTEXITCODE
    } finally { $ErrorActionPreference=$saved }
    if($code -ne 0){throw "$Exe exited with code $code"}
}
Set-Location $PSScriptRoot
try {
    $ai=Join-Path $PSScriptRoot AI
    $rt=Join-Path $ai Runtime
    $onnx=Join-Path $ai zipdepth_512x384.onnx
    $plan=Join-Path $ai zipdepth_512x384.plan
    $sdk=Join-Path $rt 'TensorRT-10.13.0.35'
    $dll=@("$sdk/lib/nvinfer_10.dll","$sdk/bin/nvinfer_10.dll") | Where-Object {Test-Path $_} | Select-Object -First 1
    $trtexec=Join-Path $sdk 'bin/trtexec.exe'
    $cudaBin=Join-Path $rt 'CUDA/bin'
    if(!(Test-Path 'FG.exe') -or !(Test-Path $onnx)){throw 'Incomplete FG release.'}
    if(!(Test-Path 'FrameWarpRuntime/nvngx_latewarp.dll')){throw 'Frame Warp runtime not present.'}
    if(!$dll -or !(Test-Path $trtexec) -or !(Test-Path $cudaBin)){throw 'Portable runtime incomplete. Run private package preparation.'}
    $env:PATH="$sdk/lib;$sdk/bin;$cudaBin;$env:PATH"
    $env:FG_TRT_DLL=$dll
    if(!(Test-Path $plan)){
        $temp="$plan.tmp"
        Write-Host 'Building FP16 engine for this GPU, once.'
        Invoke-Native $trtexec @("--onnx=$onnx","--saveEngine=$temp","--fp16","--inputIOFormats=fp32:chw","--outputIOFormats=fp32:chw","--skipInference")
        if(!(Test-Path $temp)){throw 'TensorRT engine build failed.'}
        Move-Item -LiteralPath $temp -Destination $plan -Force
    }
    Start-Process -FilePath (Join-Path $PSScriptRoot 'FG.exe') -WorkingDirectory $PSScriptRoot
}catch{
    Write-Host $_ -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
