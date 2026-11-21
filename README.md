# V15 NvFBC Enable Probe (Windows experimental)

The driver returned `NVFBC_ERROR_DRIVER_FAILURE (-5)`, `capture possible=0`, for both standard and legacy CreateEx on RTX 5060 Ti / Windows 10 22H2. This is a driver-side rejection, **not** evidence that the NvFBC 9.0 Linux direct-PID APIs are usable on Windows.

This isolated branch tests one previously untried **Windows** initialization step, NVIDIA Capture SDK 7.x `NvFBC_Enable(NVFBC_STATE_ENABLE)`, following the community [nvfbc-relay](https://github.com/garrettsparks/nvfbc-relay) technique. The enable call is attempted only when the driver reports capture disabled and not currently capturing. The app does not replace or patch the NVIDIA driver. It retains the original V15 DX9Ex -> D3D11 -> NvOFA -> DXGI pipeline.

If Enable succeeds, the app exits with a message to close and relaunch it, because the call may reset the GPU device. If Enable fails, the message reports `enable=<code>`. **-15** usually indicates insufficient administrator privileges, **-5** means driver failure. If it remains -5 even with administrator rights, this is not solvable just by copying Linux Capture SDK patches; the Windows driver has disabled NvFBC.

[Latest NVIDIA Capture SDK](https://developer.nvidia.com/capture-sdk) is Linux-only since Windows NvFBC was deprecated. Windows 10 22H2 is outside supported Windows NvFBC versions. Do not install Linux ELF driver patches or replace your signed Windows system DLLs for this test.

## Build
[AppVeyor](https://ci.appveyor.com/project/lumizabergja/wgcextrapolate) compiles this branch using `appveyor.yml`. Download the ZIP from Artifacts after successful compilation; source reconstruction hashes are checked. Run the executable normally for a baseline, then optionally as Administrator once to test the documented enable step, if you accept possible temporary display reset. The working WGC main branch and previous V15 community branch remain unchanged.
