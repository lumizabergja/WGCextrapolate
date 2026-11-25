# FG WGC 320 DX11 camera motion experiment

This build estimates one screen-space displacement per captured frame pair with a DX11 compute shader. It samples small patches from the original capture textures with a four-pixel coarse search followed by one-pixel refinement, then reuses the estimate across generated frames. Low-confidence and out-of-range estimates use zero displacement. Independently moving objects and larger mouse flicks can exhibit judder.

Build with `Build.cmd` from an x64 Visual Studio Developer Command Prompt. Warp On requires the original `FrameWarpRuntime/nvngx_latewarp.dll` beside FG.exe in the FrameWarpRuntime directory. Both modes support live Radius and Feather.

To compare against DualOFA, use CS2 capped at 80 FPS, 4×, Warp Off, and a 320 Hz display. Compare real-frame latency and displayed generated-frame cadence at identical game settings.
