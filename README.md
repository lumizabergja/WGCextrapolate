FG DXGI 320 - All-frame late latch

Extract the entire ZIP. Open an x64 Visual Studio Developer Command Prompt and run Build.cmd; launch bin\FG.exe. Select the game and enable Frame Warp before Start. Use Stop to change it. Frame Warp defaults to off.

Native legacy Streamline D3D12 presenter with DXGI capture, GPU shared color, flat depth, zero motion supplied to Latewarp, and estimated camera pose from raw mouse counts (CS2 sensitivity 1.5, default yaw/pitch). NvOFA extrapolation is unchanged. Every real or generated frame prepares resources and the next camera token first, then drains queued raw mouse input and submits the latest camera immediately before the Streamline swapchain Present. The DLLs and fixed protection mask are unchanged. Source pose is estimated externally; optical-flow extrapolation may already include camera motion. Actual GPU warp, image quality, timing and latency require live testing. No new Windows binary is included in this source archive.

GitHub Actions: the included workflow builds with MSVC on windows-latest and uploads FG-DXGI-320-AllFrameLateWarp-Windows-x64. Push the extracted source tree including its .github folder to a branch in your repository, subject to the applicable redistribution rights for the supplied runtime DLLs. Alternatively build locally using Build.cmd.

CI artifact: extract FG.exe next to the existing FrameWarpRuntime folder from the original local ZIP. The runtime binaries are intentionally not redistributed in this repository.
