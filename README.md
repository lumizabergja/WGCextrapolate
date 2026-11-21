# WGC Extrapolate — visible motion v3 (D3D11 / Windows)

Experimental **Windows Graphics Capture → D3D11 → NVIDIA Optical Flow (FAST 4x4) → extrapolation shader → DXGI flip presentation**, with Off, Dynamic, and fixed x2–x6.

## Why the previous version looked like native frames

Two different failure modes were possible:

- The scheduler previously prioritized new native frames so aggressively that it could starve generated slots at high source FPS.
- Even when it submitted an FG frame, the shader's scene-cut/residual/confidence checks could reject the optical-flow warp and return a copy of the real frame, particularly during camera movement.

**v3** removes that overaggressive shader gate and uses a cheaper single-flow-warp prediction shader. Fixed x2–x6 modes reserve at most one imminent generated presentation opportunity before accepting the latest waiting native frame. Off and Dynamic retain a real-frame-first path. There are no additional CPU pixel copies or D3D11-to-DX12 conversions.

## Identify your build

The v3 executable window title is `FG WGC v3`. Its status line reads:
`v3 WGC .../s  Real .../s  FG .../s  OF .../s`

- `OF=0` while real captures update in fixed x4: the optical-flow engine is not executing, so check mode/initialization/errors.
- `OF>0, FG=0`: a scheduler/flip-slot timing problem remains.
- `OF>0, FG>0` but still no visible motion: the shader may be receiving zero or invalid motion vectors. The FG number counts submitted draws, not actual scanout or changed pixels.

## Build in AppVeyor

The public repo's `appveyor.yml` calls `python reconstruct.py` to produce the SHA256-verified v3 source from three small patch stages (base, pacing v2, motion v3). Then it downloads Nvidia optical-flow headers, compiles on Visual Studio 2022, and should publish `FG-WGC-D3D11-LowLatency-Windows-x64.zip` in Artifacts **if successful**.

For local builds use x64 Visual Studio Developer Command Prompt, then `python reconstruct.py` and `Build-And-Pack.cmd`.

**Important:** This is an unverified source-level fix. It has passed deterministic source-reconstruction validation, not Windows compilation or actual GPU runtime tests. At 240 Hz the display can show at most 240 total frames/s; adding FG frames at 240 native FPS replaces some native display opportunities. To see stronger extrapolation, first try fixed x4 at 60 native FPS on a moving scene.
