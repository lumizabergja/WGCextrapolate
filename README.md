# WGC Extrapolate — D3D11 zero-copy, real + generated frames

Experimental **Windows Graphics Capture (WGC) → Direct3D 11 → NVIDIA Optical Flow → D3D11 presentation** application for Windows 10 22H2+ with Off / Dynamic / fixed x2–x6 extrapolation.

## Latest fix: visible generated frames

The original real-first scheduler could send 0 generated frames if WGC provided a new native frame every display slot; it also refused to initialize optical flow in fixed x2–x6 when native frame rate approached refresh rate.

The fixed x2–x6 modes now:
- Initialize NvOFA whenever a suitable real-frame pair exists, even when capture exceeds display refresh.
- Reserve **one** synthetic display opportunity after a real image; then prioritize the **newest** deferred real image on the next slot. There is no FIFO and no CPU-pixel copy.
- Fill further empty display slots with up to xN-1 extrapolations, but stop predicting across stale captures.
- Give real input precedence if the reservation is overdue (two display refresh intervals).

**Off** and **Dynamic** remain real-first, without reserving an artificial generated slot. At 240 captured FPS on a 240 Hz monitor, a generated frame can only replace a native display opportunity. With fixed x2–x6, this is an unavoidable response-vs-generated-frame tradeoff. For a full x4 ratio at 240 Hz, target about 60 native FPS; for full x6, target about 40 native FPS.

## AppVeyor public Windows build

1. Connect this **public** repository at [AppVeyor](https://ci.appveyor.com/).
2. AppVeyor automatically reads [appveyor.yml](appveyor.yml), reconstructs the exact source using [reconstruct.py](reconstruct.py), and downloads NVIDIA interface headers.
3. The Visual Studio 2022 Windows x64 build publishes `FG-WGC-D3D11-LowLatency-Windows-x64.zip` under **Artifacts**, plus `build.log` when compilation reaches MSVC.

To build locally: run `python reconstruct.py` and then `Build-And-Pack.cmd` in an **x64 Visual Studio Developer Command Prompt**.

This is a source-level fix, **not yet compiled or runtime-verified on the target GPU**. The `WGC / Native / FG` readout is a helpful presenter-submission count but does not prove every frame reached scanout, nor that click-to-photon latency improved. Some generated frames can look similar to native images in stationary scenes.
