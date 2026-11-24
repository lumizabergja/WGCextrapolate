# FG D3DKMT Native Only — 320 Hz

D3DKMT output duplication is the sole capture backend. No DXGI Desktop Duplication or WGC fallback, including on watchdog timeouts. DXGI is retained for display enumeration and presentation only. Native frame transport and keyed-mutex handshake are experimental. A successful session creation is not proof of pixel capture.

1. Extract the Windows x64 artifact, open a normal desktop application with moving content, launch FG.exe, select the window, keep Frame Warp off, then press Start. The title remains waiting until two distinct non-marker pixel samples validate the capture.
2. If capture stops, copy the complete error dialog (Ctrl+C while focused). It identifies the failed operation or the watchdog's last stage. No native frame in 4 seconds (3 seconds after validation) stops the session. The isolated worker is terminated after a 6-second heartbeat stall. Stop and game exit leave the launcher open.
3. Native mode is mandatory and target defaults to 320 Hz. No driver patching or protected-content capture. The native shared texture is copied GPU-to-GPU during operation; only startup validation reads a small pixel sample on CPU.

Optional Frame Warp requires the original FrameWarpRuntime folder beside FG.exe from the supplied LateInput package. Keep it off for capture diagnostics. Windows CI compilation is separate from live GPU validation.
