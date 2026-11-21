# WGC Extrapolate — v4 reduced-overhead D3D11 FG
Experimental Windows Graphics Capture / NVIDIA optical flow / D3D11 frame-extrapolation app.

**v3 working backup:** [v3-working-backup](../../tree/v3-working-backup). v4 deliberately retains its working x2–x6 generated-frame scheduler and the same optical-flow warp math.

**v4 changes:**
- Reuses signaled D3D11 event queries instead of creating a GPU completion query for each capture.
- Removes unnecessary vertex-shader resource/constant/sampler bindings and unused previous-frame pixel-shader SRV.
- Samples the current full-resolution texture once, rather than twice, on unprotected prediction pixels.
- Keeps WGC native texture SRVs, NvOFA 4x4 FAST, max DXGI frame latency 1, the real-frame fast path, and no app-level pixel copies or DX12 conversions.

**Build:** [AppVeyor](https://ci.appveyor.com/project/lumizabergja/wgcextrapolate) uses `appveyor.yml` and `reconstruct.py` to verify and compile v4, then publishes `FG-WGC-D3D11-LowLatency-Windows-x64.zip` if successful. Local source archives may contain fully reconstructed `src/main.cpp` and can run `Build-And-Pack.cmd` on Windows directly.

**Verification:** Source reconstruction matches SHA256 `80f95a236ee353c069971061f5fe6a4415fb4b8915dc25ca4fa39f5cdd249d12`; GPU runtime latency improvements are not yet measured. Keep the same FPS cap, display Hz, and x-multiplier when comparing v3 vs v4. Real frames and FG compete for finite 240 Hz scanout slots; visible FG at 240 native FPS cannot simultaneously display all real frames.
