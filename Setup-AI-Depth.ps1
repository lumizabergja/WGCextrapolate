param([Parameter(Mandatory=$true)][string]$TensorRT)
$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$root=(Resolve-Path $TensorRT).Path
$trtexec=@("$root/bin/trtexec.exe","$root/trtexec.exe") | Where-Object {Test-Path $_} | Select-Object -First 1
$dll=@("$root/lib/nvinfer_10.dll","$root/bin/nvinfer_10.dll","$root/nvinfer_10.dll") | Where-Object {Test-Path $_} | Select-Object -First 1
if(!$trtexec -or !$dll){throw 'Install NVIDIA TensorRT 10.13 Windows x64 SDK (including trtexec).'}
$env:PATH="$(Split-Path $dll);$(Split-Path $trtexec);$env:PATH"
$ai=Join-Path $PSScriptRoot AI
New-Item -ItemType Directory -Force $ai | Out-Null
$src=Join-Path $ai ZipDepth
if(!(Test-Path "$src/.git")){
    & git clone https://github.com/fabiotosi92/ZipDepth.git $src
    if($LASTEXITCODE){throw 'ZipDepth clone failed'}
}
& git -C $src checkout 91f3fd21e131641f51e8d35736d1958350180e3a
if($LASTEXITCODE){throw 'ZipDepth checkout failed'}
if(!(Test-Path "$ai/venv/Scripts/python.exe")){
    & python -m venv "$ai/venv"
    if($LASTEXITCODE){throw 'Python venv failed'}
}
$py="$ai/venv/Scripts/python.exe"
& $py -m pip install --upgrade pip
if($LASTEXITCODE){throw 'pip update failed'}
& $py -m pip install -r "$src/requirements.txt" onnx onnxsim
if($LASTEXITCODE){throw 'ZipDepth dependencies failed'}
& $py -m pip install -e $src --no-deps
if($LASTEXITCODE){throw 'ZipDepth install failed'}
$onnx=Join-Path $ai zipdepth_512x384.onnx
Push-Location $src
try{
    & $py scripts/export.py --ckpt checkpoints/zipdepth_base_npu.pth --npu --format onnx --width 512 --height 384 --output $onnx
    if($LASTEXITCODE){throw 'ZipDepth ONNX export failed'}
}finally{Pop-Location}
$plan=Join-Path $ai zipdepth_512x384.plan
& $trtexec "--onnx=$onnx" "--saveEngine=$plan" --fp16 --inputIOFormats=fp32:chw --outputIOFormats=fp32:chw --skipInference
if($LASTEXITCODE -or !(Test-Path $plan)){throw 'TensorRT FP16 engine build failed'}
Set-Content -LiteralPath "$ai/trt-dll.txt" -Value $dll -Encoding UTF8
Write-Host "Engine ready: $plan"
Write-Host 'Run Start-AI-FG.ps1 from this directory. Runtime compatibility is still experimental.'
