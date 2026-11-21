# V15 Clean v16 — Looking Glass DXGI Desktop Duplication experimental backend

Uses open-source Looking Glass B7 **DXGI 1.5 Desktop Duplication design**, adapted to our **same-PC D3D11/NvOFA** extrapolator, without using IVSHMEM or Looking Glass's Windows host executable.

Pipeline:
`Selected game monitor → DXGI Output5::DuplicateOutput1 (BGRA8) → one GPU-only CopyResource into reusable D3D11 textures → NvOFA 4x4 FAST → original V15 Clean v16 HLSL → DXGI flip-discard presenter`

- No Nvidia NvFBC, DLL hacks, WGC, D3D9Ex, CPU readback, D3D11on12, VM, injection or Looking Glass client dependency.
- Single NVIDIA GPU and display monitor selected from game HWND.
- Original V15 HLSL shader verified unchanged. Original click-through UI/Start/Stop, dynamic and x2-x6 remain. One synthetic slot reserved in fixed modes, real frames have priority afterward.
- GPU copy completion fences and D3D11 multithread protection. Skip pointer-only DXGI updates. No copy from flow output to present backbuffer.
- Overlay capture exclusion is applied before setting WS_EX_LAYERED to avoid Win10 WDA failures.
- Captures full monitor image, not game PID. At 4:3 stretched CS2 it captures the stretched display image, and NvOFA works at monitor resolution.
- DXGI can add compositor latency and does not guarantee faster end-to-end capture than WGC; no claimed measurements yet.

**Windows x64 build:** [AppVeyor CI](https://ci.appveyor.com/project/lumizabergja/wgcextrapolate) automatically compiles this branch if build succeeds, publishing `FG-V15-LookingGlass-DXGI-Windows-x64.zip` in Artifacts.

For local compilation: run `python -m pip install brotli==1.1.0`, `python prepare_source.py`, `powershell -ExecutionPolicy Bypass -File Download-Headers.ps1`, and `Build.cmd` from an x64 Visual Studio Developer Command Prompt.

Port based on the user's original V15 Clean v16 source. Capture design reference: [gnif/LookingGlass DXGI source](https://github.com/gnif/LookingGlass/blob/master/host/platform/Windows/capture/DXGI/src/dxgi.c). No Looking Glass code copied verbatim.

**Status:** Experimental source patch; compilation and live RTX 5060 Ti runtime not yet verified. Compare x4 at 60 native FPS and then x2 at 120 native FPS, both at 240 Hz. Independent Flip is controlled by Windows and not guaranteed by code.
