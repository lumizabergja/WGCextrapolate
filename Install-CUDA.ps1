$ErrorActionPreference = 'Stop'
$cuda = Join-Path $env:ProgramFiles 'NVIDIA GPU Computing Toolkit\CUDA\v12.8'
if (!(Test-Path "$cuda\bin\nvcc.exe") -or !(Test-Path "$cuda\lib\x64\cudart_static.lib")) {
    $installer = Join-Path $env:TEMP 'cuda_12.8.1_windows_network.exe'
    $url = 'https://developer.download.nvidia.com/compute/cuda/12.8.1/network_installers/cuda_12.8.1_windows_network.exe'
    & curl.exe --fail --location --retry 3 --output $installer $url
    if ($LASTEXITCODE -ne 0) { throw 'CUDA installer download failed' }
    $process = Start-Process -FilePath $installer -ArgumentList '-s','-n','nvcc_12.8','cudart_12.8','thrust_12.8' -Wait -PassThru
    if ($process.ExitCode -notin @(0,3010)) { throw "CUDA setup exit code: $($process.ExitCode)" }
}
if (!(Test-Path "$cuda\bin\nvcc.exe") -or !(Test-Path "$cuda\lib\x64\cudart_static.lib")) {
    throw 'CUDA Toolkit 12.8 compiler or static runtime missing'
}
$env:CUDA_PATH = $cuda
& "$cuda\bin\nvcc.exe" --version
if ($LASTEXITCODE -ne 0) { throw 'CUDA compiler unavailable' }
