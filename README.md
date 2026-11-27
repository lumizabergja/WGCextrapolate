# FG CUDA Ultra Pipeline

This experimental revision keeps NvFBC CUDA PID capture, the existing UI, Dynamic 320, x2-x6, center protection and full-resolution NvOFA 4x4 FAST cost OFF.

- Persistent D3D11 shared KMT textures, imported CUDA surface arrays and per-slot D3D11/CUDA timeline fences. If texture import or fence initialization fails, the original mapped-buffer presenter is selected before any output submission. Status indicates the chosen route. No runtime recovery after a GPU submission failure.
- Separate luma preparation stream: prepare each selected frame before its OFA pair is requested, including while prior OFA work is active. Slot retirement also waits for luma completion.
- Source-resolution photometric confidence map per pair, reused across predictions. Flow discontinuity and border/center protection remain per output. Bilinear confidence lookup approximates the previous exact color-residual calculation.
- Separate real-frame kernel, phase-locked output, bounded event waits and limited prediction lead based on deadline and measured CPU submission time; not a scanout measurement.

Build with Build.cmd --blackwell, then Test.cmd. AppVeyor produces Windows and source ZIPs. Real GPU synchronization, KMT texture allocation-size compatibility and latency require testing on the target PC. No claim of native 320 FPS input latency.
