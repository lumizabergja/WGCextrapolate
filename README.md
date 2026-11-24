# FG WGC 320 — Shared zero-copy / Both Modes

Both Warp OFF and Warp ON use identical WGC HWND capture, native D3D11 direct surface leases, NvOFA 4x4 FAST (cost OFF), real-frame shader, deadline scheduler, and fully opaque layered NOREDIRECTIONBITMAP window configuration. Dynamic 320 remains the default. No application capture-history copy or fallback. Direct WGC shader binding and NvOFA registration must succeed; otherwise report the error. Capture indicators are preserved.

The renderer rechecks capture before optical flow; uses an immediate-context D3D11 fence after flow submission; does not queue a second flow calculation while a previous one is outstanding; checks readiness before fake submission without a blocking CPU fence wait; recalculates prediction age after acquiring a display slot. Retirement signals are batched into the next submission when possible. These changes avoid deliberate CPU waits for motion, but D3D11 implicit GPU ordering and driver-side stalls may remain. This is not an independent asynchronous D3D12 OFA queue.

Warp OFF presents through native D3D11. Warp ON preserves the original Streamline 2.7 D3D12 presenter and real-only late mouse correction, including required shared-resource synchronization and color copies. Neither mode can force Independent Flip. Verify with PresentMon and confirm mouse click-through on the target PC.

Build with MSVC x64 and a recent Windows SDK. Place the original six FrameWarpRuntime DLLs next to FG.exe under FrameWarpRuntime/. Actual compatibility and latency are untested.
