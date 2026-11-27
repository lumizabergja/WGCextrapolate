$ErrorActionPreference = 'Stop'
$cuda = Join-Path $env:ProgramFiles 'NVIDIA GPU Computing Toolkit\CUDA\v12.8'
if (!(Test-Path "$cuda\bin\nvcc.exe") -or !(Test-Path "$cuda\lib\x64\cudart_static.lib")) {
    $installer = Join-Path $env:TEMP 'cuda_12.8.1_windows_network.exe'
    $url = 'https://developer.download.nvidia.com/compute/cuda/12.8.1/network_installers/cuda_12.8.1_windows_network.exe'
    Write-Host 'Downloading CUDA 12.8.1 network installer'
    $curlArgs = @('--silent','--show-error','--fail','--location','--retry','3','--output',('"' + $installer + '"'),('"' + $url + '"'))
    $download = Start-Process -FilePath (Get-Command curl.exe -ErrorAction Stop).Source -ArgumentList $curlArgs -NoNewWindow -Wait -PassThru
    if ($download.ExitCode -ne 0) { throw "CUDA installer download failed: curl exit $($download.ExitCode)" }
    if (!(Test-Path $installer) -or (Get-Item $installer).Length -eq 0) { throw 'CUDA installer file missing or empty' }
    $process = Start-Process -FilePath $installer -ArgumentList '-s','-n','nvcc_12.8','cudart_12.8','thrust_12.8' -Wait -PassThru
    if ($process.ExitCode -notin @(0,3010)) { throw "CUDA setup exit code: $($process.ExitCode)" }
}
if (!(Test-Path "$cuda\bin\nvcc.exe") -or !(Test-Path "$cuda\lib\x64\cudart_static.lib")) { throw 'CUDA compiler or runtime missing' }
$env:CUDA_PATH = $cuda
& "$cuda\bin\nvcc.exe" --version
if ($LASTEXITCODE -ne 0) { throw 'CUDA compiler unavailable' }
