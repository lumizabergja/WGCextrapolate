# FG WGC 320 – Low-latency / Windows 11 26H2

Uses direct HWND WGC capture on the display adapter with two free-threaded capture buffers, zero requested minimum update interval, latest-frame drain, cursor disabled, and OS capture indicators intact. No Desktop Duplication fallback.

D3D11 remains the native capture/presenter/optical-flow API to avoid adding D3D11-to-D3D12/Vulkan interop synchronization. Capture history textures are preallocated and registered with NvOFA 4x4 FAST; one GPU texture copy per new source, no CPU readback or per-frame OFA registration. Output uses flip-discard, two buffers, waitable swapchain max latency 1, Present(0) with tearing if supported. Optional existing Frame Warp remains separate; leave it OFF for lowest-overhead path.

Pacing now separates WGC source timestamps (motion interval) from real submission times (prediction slots). An event-driven QPC scheduler waits for WGC, presentation availability and high-resolution prediction timer, avoids catch-up bursts and stale predictions, and always gives new real frames priority. Dynamic 320 can use up to 12 predictions per source interval at very low input FPS; static Off/x2–x6 unchanged. Predictions are limited to one measured source interval. No guarantee of independent flip, exact output FPS, or actual latency; benchmark on hardware. Layered click-through retained for input compatibility.

Build via MSVC Windows x64 on GitHub Actions. Extract FG.exe alongside the original FrameWarpRuntime folder only if using optional Frame Warp.
