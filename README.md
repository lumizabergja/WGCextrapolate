# FG WGC 320 – Strict zero-copy / Windows 11 26H2

Experimental native D3D11 WGC path. Direct HWND capture, free-threaded pool with five surfaces, latest-frame drain, minimum update interval zero where supported. Original WGC BGRA8 texture is sampled directly and registered with NVIDIA Optical Flow SDK on the same device. No application-side intermediate full-frame copies, CPU readback, or graphics API interop. Real and predicted frames render directly to flip-discard swapchain; rendering and any OS/driver-internal copies still occur.

Active current/previous leases plus bounded retiring leases; GPU fence completion precedes NvOFA unregistration and WGC frame return. Registrations are retained only for a live lease, never across pool check-in. Nonblocking retirement during rendering. Shutdown stops capture and drains GPU work (bounded wait). Strict format/bind flags and OFA registration checks fail visibly; no fallback. WGC/driver registration compatibility and GPU synchronization behavior require live validation on the target RTX 5060 Ti.

Retains event-driven pacing, Dynamic 320, Off–x6 static, NvOFA 4x4 FAST cost OFF and click-through overlay. Frame Warp is disabled because it introduces another copy/interop path. Build on GitHub Actions, Windows x64. No runtime DLLs required for this mode. Actual latency, smoothness and independent flip are not verified by compilation.
