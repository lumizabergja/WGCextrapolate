# FG EarlyFlow NGX Predict3

Experimental external native DX11 NGX latewarp (Reflex 2 Frame Warp path), not a game-integrated NVIDIA Reflex 2 implementation. WGC capture, NvOFA FAST 4x4 cost OFF, Dynamic 320. No custom late camera pixel reprojection: optical flow extrapolates motion, then NGX is the only late camera transform.

- Dedicated Raw Input message thread, intended for a 1000 Hz mouse. Windows message receipt timestamps are not hardware timestamps; this does not change mouse polling configuration.
- Real and generated frames each register their input image and evaluate NGX once more with a fresh target camera pose. Adaptive prediction up to 3 ms, reversal/deceleration/staleness suppression, bounded acceleration and 4-degree total target correction.
- For flow-extrapolated frames estimate their source camera using preceding mouse delta times the extrapolation horizon; for camera-only fallback use the latest real source pose. Source timestamp uses an assumed 5 ms input-to-capture offset. No actual game pose or depth.
- Automatic mask generated in the existing extrapolation draw using MRT: screen-space appearance, flow disagreement with estimated camera motion, predicted source coordinates, and local neighbor expansion. Moving weapon keeps local flow rather than freezing a rectangular region. Final draw composites protected unwarped (or extrapolated) pixels over NGX output. Fixed manual center radius/feather optional. The heuristic cannot perfectly separate HUD, weapon, and background from flattened capture. Edges can flicker and low-confidence foreground may reuse real pixels. No guaranteed latency benefit.
- NGX uses synthetic depth/motion and experimental undocumented latewarp parameters. It performs two evaluations per output plus a composite draw; higher GPU work than the previous custom shader. Runtime behavior, latency and visual accuracy require Windows GPU testing.
- Default checkbox NGX Frame Warp + Predict3 + Auto HUD is ON; unchecking bypasses NGX camera warp. Initial history warmup or unavailable nonblocking pose can bypass warp for a frame. Off multiplier still allows real-frame warp if enabled.

## Run
Build.cmd restores the original NGX SDK download. Copy the original compatible FrameWarpRuntime directory (including nvngx_latewarp.dll) from your working NGX package beside FG.exe. It is not bundled by GitHub Actions. Missing runtime causes an initialization error. Use CS2 800 DPI, sensitivity 1.5, 1000 Hz mouse polling, Dynamic 320, center radius/feather zero to test the automatic mask. Actual game input is never modified.
