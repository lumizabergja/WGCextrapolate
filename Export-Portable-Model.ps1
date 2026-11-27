$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$src=Join-Path $pwd 'zipdepth-export'
& git clone https://github.com/fabiotosi92/ZipDepth.git $src
if($LASTEXITCODE){throw 'ZipDepth clone failed'}
& git -C $src checkout 91f3fd21e131641f51e8d35736d1958350180e3a
if($LASTEXITCODE){throw 'ZipDepth checkout failed'}
$python='C:\Python311-x64\python.exe'
if(!(Test-Path $python)){$python=(Get-Command python.exe).Source}
& $python -m pip install --index-url https://download.pytorch.org/whl/cpu 'torch==2.9.1'
if($LASTEXITCODE){throw 'CPU PyTorch install failed'}
& $python -m pip install 'onnx==1.19.1' 'numpy<3'
if($LASTEXITCODE){throw 'ONNX dependencies failed'}
$script=Join-Path $src 'scripts/export.py'
$code=(Get-Content $script -Raw).Replace('opset_version=opset,','opset_version=opset, dynamo=False,')
Set-Content -LiteralPath $script -Value $code -Encoding UTF8
$dest=Join-Path $pwd 'portable/AI'
New-Item -ItemType Directory -Force $dest | Out-Null
Push-Location $src
try{
    & $python scripts/export.py --ckpt checkpoints/zipdepth_base_npu.pth --npu --format onnx --width 512 --height 384 --output "$dest/zipdepth_512x384.onnx"
    if($LASTEXITCODE){throw 'ZipDepth export failed'}
    & $python -c "import onnx,sys; m=onnx.load(sys.argv[1]); onnx.checker.check_model(m); print('ONNX checked:',m.graph.input[0].name,m.graph.output[0].name)" "$dest/zipdepth_512x384.onnx"
    if($LASTEXITCODE){throw 'ONNX validation failed'}
    Copy-Item LICENSE "$dest/ZipDepth-LICENSE.txt"
}finally{Pop-Location}
