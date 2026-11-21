# WGC + NvOFA 4x4 FAST extrapolation — V15 Clean V16 motion integration

## V15 Clean V16 motion and pacing integration
This branch keeps the **fresh WGC HWND window capture and NvOFA FAST 4x4 / output cost OFF** backend. It ports the original V15 Clean V16 shader from the repository's `v15-nvfbc-community` source, including 4-neighbor optical-flow interpolation, current/previous color consistency, 5-point scene-cut detection, bounded motion, and the exact scene/occlusion blending logic. Default center protection is disabled (radius 0, feather 0).
  
Unlike the early fresh shader, flow is calculated **current -> previous**, which matches V15's shader sampling direction `uv + horizon*flow`. Real frames use horizon 0, generated frames use source-age extrapolation >0, both after NvOFA and through the same shader.

The presenter uses a 2-buffer waitable DXGI flip-discard swapchain, maximum frame latency 1, a fixed backbuffer RTV, `Present(0)` and a 1-slot software scheduling policy. The latest WGC frame replaces stale captures; generated slots are spaced by the measured source interval and limited by the display refresh. Real frames take priority. For a 240 Hz monitor, test x4 with a source near 60 FPS; there cannot be 960 distinct displayed frames from a 240 FPS x4 input on that monitor.

The diagnostic menu still provides a magenta generated-frame marker and an NvOFA flow-vector visualization. Submission counters do not guarantee physical scanout. **This build is an experimental integration; hardware latency and quality are not yet verified.**


**This implementation is written from scratch using public Microsoft and NVIDIA documentation.** It contains no old project source, presenters, or shaders.

## Target
- Windows 10 22H2 (build 19045); GPU: NVIDIA RTX 5060 Ti 8 GB; driver 576.88.
- Capture: Windows.Graphics.Capture -> CreateFreeThreaded 2-buffer pool, HWND selection, newest frame only.
- D3D11 GPU-resident textures. NvOFA requires persistent registered input surfaces; one GPU CopyResource per real frame when the driver accepts BGRA8, otherwise a GPU-only RGBA conversion pass. There is **no CPU pixel readback**. This is NOT zero-copy end-to-end.
- NvOFA SDK 5.0 D3D11, FAST preset, 4x4 grid, `enableOutputCost = NV_OF_FALSE`.
- Extrapolation: signed S10.5 vector decoding, luma-guided flow smoothing, bounded backward warp, previous-frame mismatch/occlusion fallback, clip-edge protection. Off/x2/x3/x4.
- Presentation: D3D11 flip-discard swap chain, tearing if allowed, DXGI maximum latency 1. Both real and generated frames execute NvOFA FAST 4x4 and the same HLSL motion-extrapolation shader before the same DXGI Present(0); real frames use extrapolation factor 0, generated frames use factors above 0.
- Live mode selector, window Refresh/Start/Stop, Ctrl+Alt+F8 stop, fullscreen output option.

## Build
On a Visual Studio 2022/2026 x64 Developer Command Prompt:

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File Download-Headers.ps1
build.cmd
```

The `appveyor.yml` creates an artifact ZIP on a Windows runner. No Python.


## Checking whether generated frames are actually visible
- The default diagnostic view is **Magenta marker on generated frames**. A magenta bar in the upper-left corner is drawn by the HLSL pixel shader only when its extrapolation factor is above zero. Select **Normal image** to disable the marker for normal quality testing.
- Select **Visualize NvOFA motion vectors** to inspect the output of optical flow on both real and generated frames. Uniform gray/dark means little detected motion; colors changing as the scene moves indicate nonzero motion vectors.
- The title displays **WGC/s**, **FG submitted/s**, and **NvOFA/s**. *Submitted* means the application called DXGI Present; Windows may replace frames before scanout.
- On a 240 Hz screen, test with a moving window capped near **60 real FPS** and select **x4** (or 120 FPS and x2). If the source already updates at 240 FPS, attempting x4 would require 960 distinct displays per second, which a 240 Hz panel cannot show.
- If FG submitted/s remains zero, the scheduler/capture cadence is preventing generated presents. If FG submitted/s is positive but the magenta bar never appears, investigate DXGI queue replacement, compositor mode, and actual scanout. If the bar appears but the scene still looks native, switch to the flow-vector diagnostic to check for weak/zero motion or the shader's occlusion fallback.

## Accuracy and limitations
- **Experimental; not hardware verified.** A completed MSVC build is not a substitute for GPU testing.
- WGC's own internal capture can be buffered/copied by Windows and is not provably zero-copy. Windows 10 has no `MinUpdateInterval` property.
- Hardware/NVIDIA format support is checked at runtime; unsupported formats return explicit errors instead of silently falling back.
- The capture and presenter can still be DWM-composed: fullscreen flip-model does not guarantee Independent Flip. Click-through depends on Windows hit testing.
- Extrapolation cannot reveal correct hidden geometry without depth; a color-based disocclusion gate reduces artifacts but does not eliminate them.
- NvOFA SDK 5.0's FAST preset trades some motion-vector quality for throughput. If the D3D11 device/format negotiation is not compatible with the target driver, the app stops and reports an error.
- Shared processing mode deliberately runs NvOFA for real frames too, including Off mode; it must wait for a complete frame pair, so the first captured real frame initializes history and is not presented. Unlike a real-frame bypass, this adds flow processing to real frames and can increase latency. D3D11/NvOFA execution is asynchronous and actual synchronization/performance must be hardware-tested.
- Capture/scanout FPS and end-to-end input latency must be measured on the actual system; not claimed here.

## Public references
- https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture
- https://github.com/robmikh/Win32CaptureSample
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/index.html
- https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
- SDK 5.0 interface mirror: https://github.com/SlimRG/NvidiaOF5
