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
$src=Join-Path $pwd 'zipdepth-export'
if(Test-Path $src){Remove-Item -LiteralPath $src -Recurse -Force}
Invoke-Native git @('clone','--quiet','https://github.com/fabiotosi92/ZipDepth.git',$src)
Invoke-Native git @('-C',$src,'checkout','--quiet','91f3fd21e131641f51e8d35736d1958350180e3a')
$python='C:\Python311-x64\python.exe'
if(!(Test-Path $python)){$python=(Get-Command python.exe).Source}
Invoke-Native $python @('-m','pip','install','--disable-pip-version-check','--index-url','https://download.pytorch.org/whl/cpu','torch==2.9.1')
Invoke-Native $python @('-m','pip','install','--disable-pip-version-check','onnx==1.19.1','numpy<3')
$script=Join-Path $src 'scripts/export.py'
$code=(Get-Content $script -Raw).Replace('opset_version=opset,','opset_version=opset, dynamo=False,')
Set-Content -LiteralPath $script -Value $code -Encoding UTF8
$dest=Join-Path $pwd 'portable/AI'
New-Item -ItemType Directory -Force $dest | Out-Null
Push-Location $src
try{
    Invoke-Native $python @('scripts/export.py','--ckpt','checkpoints/zipdepth_base_npu.pth','--npu','--format','onnx','--width','512','--height','384','--output',"$dest/zipdepth_512x384.onnx")
    Invoke-Native $python @('-c',"import onnx,sys; m=onnx.load(sys.argv[1]); onnx.checker.check_model(m); print('ONNX checked:',m.graph.input[0].name,m.graph.output[0].name)","$dest/zipdepth_512x384.onnx")
    Copy-Item LICENSE "$dest/ZipDepth-LICENSE.txt"
}finally{Pop-Location}
