# Looking Glass-style DXGI capture-only v2 — fullscreen + click-through

This branch is based on the **working capture-only v1**. It changes the window and output presentation **only**. No optical flow, interpolation, motion vectors, NvFBC, DLL injection, API capture fallback, or game frame generation.

**Start:** double-click `FG-LG-Fullscreen.exe`. It creates a full-monitor, always-on-top, borderless, fully opaque image that passes clicks and keyboard focus to the game underneath.

**Close:** **Ctrl+Alt+F8**. Esc no longer works because the full-screen window never accepts focus. Mouse input remains with the underlying application.

The flip swapchain is created before making the window topmost and fullscreen (Microsoft DXGI sample startup sequence).\n\nThe capture path is unchanged: `IDXGIOutput5::DuplicateOutput1` (or DXGI standard DDA for compatibility) → single GPU-only D3D11 `CopyResource` → original preview pixel shader → flip-discard `Present(0, DO_NOT_WAIT)`. Output is stretched to the full selected display, with no letterbox.

Window capture exclusion (`WDA_EXCLUDEFROMCAPTURE`) is applied **before** setting the layered window style; `WCA_EXCLUDED_FROM_DDA` is requested as an optional DDA protection. A background display mode change updates the fullscreen window and rebuilds the backbuffer.

**Select another monitor:** launch with `--monitor=2`.

**Known risk:** a layered topmost HWND may affect DXGI flip presentation and independent flip eligibility. This source patch is checksum verified but needs actual Windows x64 compilation and user runtime testing. If the overlay is black, fails to launch, or shows a recursive image, revert to [capture-only v1](../lookingglass-dxgi-capture-only); do not assume self-capture prevention has worked.

Build in an x64 MSVC Developer Command Prompt: `python prepare_source.py` then `Build.cmd`. No Python is needed to run the compiled EXE. AppVeyor builds and publishes `FG-LookingGlass-Fullscreen-ClickThrough-Windows-x64.zip`.
