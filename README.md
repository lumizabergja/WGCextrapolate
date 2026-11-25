# FG EarlyFlow Predict3 Pro

External WGC / D3D11 presenter, no injection. Reflex 2/NGX removed from executable and build dependencies. Late input and automatic screen-space layer protection default on. Existing manual center radius/feather remain optional.

- Dedicated Raw Input message thread; nonblocking render snapshot.
- Camera reprojection for real and generated frames, except Off mode.
- Camera-only fallback when matching NvOFA flow is not ready; no extra GPU pass or frame queue. Shared D3D11 driver scheduling can still serialize GPU work.
- Prediction budget based on half a display refresh plus 0.35 ms, capped at 3 ms; not a measured scanout phase.
- Per-draw sparse image alignment selection among estimated 2, 5 and 8 ms capture/input offsets; no game input timestamp available.
- Bounded acceleration during coherent speed increases; existing reversal/staleness guards retained.
- Per-pixel HUD/weapon heuristic uses screen-space correspondence and flow disagreement with estimated camera motion. Moving foreground uses its own flow where reliable, not a fixed region. With unreliable/missing flow, newest source pixels may be reused. No semantic masks/depth; flicker reduction is not guaranteed.

Use 800 DPI, 1.5 sensitivity, 1000 Hz; Dynamic 320. Keep center radius and feather at zero to test automatic protection. Disable Late input + Auto HUD / weapon for comparison. Prediction does not change firing or simulation latency. Windows compilation and on-device image quality must be verified. Build.cmd requires MSVC x64 and recent Windows SDK; no NGX download.

GitHub Actions runs CPU prediction tests, MSVC x64 compilation and offline HLSL validation before artifact upload.
