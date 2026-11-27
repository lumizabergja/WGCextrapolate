# FG CUDA AI Reflex2 Depth — Stage 1 experimental

Actual ZipDepth model path using TensorRT FP16 execution and static 512x384 RGB FP32 input/output. NvFBC capture, overlapping luma, NvOFA 4x4 FAST cost OFF, original CUDA output, direct NGX D3D11 Latewarp and adaptive 0-5 ms input retained. No game injection.

## Install
- Install official Windows x64 TensorRT 10.13 SDK compatible with the pinned headers and CUDA 12.x, Python, Git, and any required NVIDIA runtime DLLs. TensorRT SDK headers are pinned to b8db91e15be2cae4465ac17fab19e0f969e45407; the build prints their version. The matching runtime/engine is required.
- Extract the original FrameWarpRuntime folder beside FG.exe (nvngx_latewarp.dll).
- In PowerShell from the executable folder run: .\Setup-AI-Depth.ps1 -TensorRT 'C:\path\to\TensorRT-10.13.x'
- Setup downloads the official ZipDepth source/checkpoint (pinned commit), exports ONNX with unfold-free checkpoint, builds a device-local FP16 TensorRT plan, and records the runtime DLL path. Run .\Start-AI-FG.ps1. No model, plan, proprietary SDK or runtime DLL is bundled.
- Disable AI depth + NVIDIA Frame Warp to use the original Safe Overlap Luma rendering route (no depth dispatch). If initialization fails, stop, untick and restart.

## Implemented
One in-flight depth inference on a separate low-priority CUDA stream, source sequence ownership until completion. No waiting for unfinished inference; direct presentation when depth is missing. A completed depth tensor is reused for generated frames and mapped through the same approximate flow/confidence coordinates as the original color extrapolator. Depth is written into registered D3D11 output buffers and rendered into the NGX depth texture. RGB preprocessing verified against upstream predictor. No CPU frame readback. TensorRT enqueue occurs on render thread; GPU execution is async, but initialization and host enqueue can stall. Low-priority stream does not guarantee no contention with CS2. Depth estimate is affine-invariant inverse depth, normalized with a sampled per-frame robust range to an arbitrary 0.5–50 scene scale, not actual metric camera depth. No temporal scale alignment yet. Fixed 4:3 model aspect, best suited to CS2 4:3. NGX still uses estimated Raw Input camera rotation, zero MV resource and existing heuristic HUD/gun masks, not trained AI segmentation. No native NVAPI Reflex scheduling yet. No claim of complete native Reflex 2 integration or 3 ms latency. NGX evaluation still requires two evaluations per warped output. GPU/driver/NGX validation required, including whether TensorRT supports the private NvFBC CUDA context. AppVeyor compiles only and runs CPU tests, not GPU inference. 

## Portable release
AppVeyor exports the actual ONNX model at build time. Start-FG.cmd launches with local NVIDIA runtime libraries and generates a GPU-specific engine if missing. The private preparation package must supply the user's original FrameWarpRuntime and official TensorRT/CUDA dependencies; these are not published in this public repo. Once prepared, launch requires no network or Python. GPU inference and NGX compatibility are unverified.
