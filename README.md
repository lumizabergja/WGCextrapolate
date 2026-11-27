# FG V15 v16 - deadline / texture CUDA revision

Run FG.exe, select the game, select Off, x2-x6 or Dynamic, and Start. Dynamic defaults to 320 FPS. Working NvFBC CUDA PID capture is required. The UI, center radius/feather, D3D11 flip-model presenter and NvOFA 4x4 FAST (cost off) remain unchanged.

Changes from the supplied Early NvOFA source: phase-locked deadline pacing without integer Dynamic rounding, latest-source selection after a DXGI queue slot becomes available, actual frame-pair interval for extrapolation, and optional CUDA hardware bilinear sampling for BGRA and motion vectors. Incompatible texture pitches use the original manual sampling. No CPU frame copy is introduced. The old D3D11 map/unmap path is retained; persistent graphics sharing, half-resolution flow and CUDA graphs are not included in this revision.

Build: open an x64 Visual Studio Developer Command Prompt, set CUDA_PATH to the CUDA Toolkit directory and run Build.cmd. PTX is regenerated on every build because the kernel argument ABI changed. Run Build.cmd --blackwell with CUDA 12.8+ to embed an SM 120 cubin alongside the generic PTX. The application loads the cubin if supported and otherwise loads PTX. Test.cmd runs CPU simulation tests; it does not test the NVIDIA driver or actual display cadence.
