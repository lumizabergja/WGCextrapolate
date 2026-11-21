# WGC + NvOFA 4x4 FAST extrapolation — CLEAN implementation

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
