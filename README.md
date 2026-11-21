# Looking Glass-style DXGI Desktop Duplication — Capture ONLY v1

**Isolated, experimental, no FG.** Purpose: verify live desktop capture and presentation stability on Windows 10 22H2 with an NVIDIA RTX 5060 Ti **before** adding NvOFA FAST 4x4 or extrapolation.

This is NOT the official Looking Glass host or its B7 default D12 capture backend. It adapts the DXGI D3D11 Desktop Duplication design, for same-PC presentation only.

Pipeline: `Desktop output → DXGI Output5::DuplicateOutput1 (or standard DuplicateOutput) → one D3D11 GPU CopyResource → shader preview`.

- No NvOFA, confidence costs, generated frames, x4 scheduling, NVIDIA NvFBC, WGC, CUDA, cross-API copies, IVSHMEM, CPU readback or game injection.
- Single DX11 device, one thread, no locks, bounded non-blocking `AcquireNextFrame(0)`, no CPU/GPU fence wait, `Present(0, DO_NOT_WAIT)` with frame latency 1.
- Small 960x540 preview with live capture and preview frame-rate counters. Uses the primary attached monitor by default and ignores mouse-only updates.
- Windows display-affinity exclusion before showing preview to prevent recursive same-monitor screen capture. Fails explicitly if exclusion is unsupported. Optional DDA-specific exclusion is nonfatal.
- Automatic duplication retry on access loss/display mode transitions. Escape or window X exits.

## Run

1. Open the public [AppVeyor build](https://ci.appveyor.com/project/lumizabergja/wgcextrapolate) on branch `lookingglass-dxgi-capture-only`; download `FG-LookingGlass-CaptureOnly-Windows-x64.zip` from Artifacts **only after a successful build**.
2. Run `FG-LG-Capture.exe`. Move content behind the preview and confirm the picture moves and **Captured/s** continues changing. The preview itself may be excluded from capture, which is expected.
3. To capture another display: run `FG-LG-Capture.exe --monitor=2` (1-based physical outputs).
4. Test CS2 **borderless/windowed** before exclusive fullscreen; exclusive mode can interrupt desktop duplication. The title says `RETRYING DXGI` while duplication cannot start.

**Important:** counters are source updates and successful Preview `Present()` calls, **not** measured click-to-photon latency or confirmed scanout. This is source code, not Windows/GPU runtime verified yet.

## Build locally

MSVC 2022+ x64 Developer Command Prompt: `python prepare_source.py`, then `Build.cmd`. No Python packages, NVIDIA headers or SDK dependencies. Source reconstruction is SHA256-checked. User's complete readable `src/main.cpp` is included in the source ZIP; GitHub branch stores its verified compressed text to avoid duplicating large source contents.

Next, after stable capture is confirmed: add **NvOFA 4x4 FAST, confidence cost OFF** in a *separate* build; only then test extrapolation shaders.
