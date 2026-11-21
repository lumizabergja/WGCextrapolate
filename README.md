# WGC Extrapolate — experimental D3D11 low-latency frame generation

Experimental **Windows Graphics Capture (WGC) → Direct3D 11 → NVIDIA Optical Flow → D3D11 presentation** C++ application, featuring Off–x6 extrapolation controls and real-frame prioritization.

This is a **source-only experimental project**, not a verified release. Windows 10 22H2 and RTX 50-series runtime behavior, optical-flow interoperability, latency, and displayed generated frames require testing on compatible hardware.

## Build on AppVeyor (public repository)

1. Add this repository as a project at [AppVeyor CI](https://ci.appveyor.com/).
2. Use the root `appveyor.yml` configuration (Visual Studio 2022 / Windows x64).
3. After a successful compile, download the `FG-WGC-D3D11-LowLatency-Windows-x64.zip` file from **Artifacts**.

The build first reconstructs the *exact optimized C++ source* from `src/main.cpp` (the complete baseline) and `src/ops.b64` (the bundled optimization delta); SHA256 verification prevents accidentally building an unexpected version. Both assets are in this **public repository**. No private repo access, proprietary NVIDIA binaries, or driver patch is required by the build scripts. The NVIDIA optical-flow interface headers are downloaded separately during the build.

For local compilation, open an **x64 Visual Studio Developer Command Prompt**, run `python reconstruct.py`, then `Build-And-Pack.cmd`.

**Warning:** The optimizations and zero-CPU-copy intent are not proof of end-to-end zero-copy or lower click-to-photon latency. The initial Windows compile may expose SDK/API compatibility errors, which should be fixed based on the resulting `build.log`.
