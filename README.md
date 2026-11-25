# FG for CS2

Extract the entire Windows x64 ZIP and run FG.exe. Choose the CS2 window, select Dynamic and a target of 320, and press Start. The PC needs the Windows x64 Visual C++ runtime and an NVIDIA driver supporting D3D11 optical flow. The optional NGX Frame Warp mode uses FrameWarpRuntime/nvngx_latewarp.dll.

## Mouse input

The CS2 mouse input selector is read when the session starts. It applies to the foreground cs2.exe process while the Windows cursor is hidden. Use Off for the previous optical-flow-only generated frames. Latest input is the default. Predict up to 1 ms adds a short lead for recent, consistent mouse reports. Stops, reversals and irregular report spacing suppress the lead.

Latest input and prediction work with Frame Warp off. Each real or generated draw can rotate the captured view using mouse movement received since its WGC timestamp. Generated frames keep local optical flow after subtracting the estimated camera component. When flow is late, a scheduled output can use the latest image and changed mouse pose alone. With NGX Frame Warp on, real frames use NGX with measured input and generated frames use the shader. Center radius and feather are in output pixels.

Calibration assumes CS2 sensitivity 1.5, default yaw and pitch scales and the normal unscoped FOV. The shader uses the capture aspect before stretching to the monitor. Raw counts already include mouse DPI. Scoped views, recoil and the exact camera pose used by CS2 are unavailable to external capture. The WGC timestamp describes the compositor image and the mouse timestamps describe delivery to FG. The visual correction is approximate, especially on abrupt turns and at exposed edges. Game simulation, firing and networking are controlled by CS2. Dynamic 320 is a pacing target. End-to-end latency requires a measurement on the actual PC and monitor.

## Build and verify

From an x64 Visual Studio 2022 developer prompt with Windows SDK 10.0.26100 or newer, run Build.cmd. The script downloads the pinned NGX development headers and import library. Run tests\Run.cmd to check timestamp selection, optional mouse prediction and real shader output on the D3D11 software renderer. If rebuilding with optional NGX Frame Warp, copy the FrameWarpRuntime directory from the Windows package beside bin/FG.exe.
