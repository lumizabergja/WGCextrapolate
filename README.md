# FG V15 Clean v16 — Community NvFBC compatibility branch

**Experimental. Not yet verified on the user's RTX 5060 Ti or Windows 10 22H2.**

This branch compiles the original V15 v16 DX9Ex -> shared D3D11 -> NvOFA -> DXGI code via SHA-verified reconstruction from the original source patches, then applies a small community NvFBC compatibility patch (hash fb445337af61c569883a67b83fa27587387745425c02775bd1165476edbeecee).

Changes: Windows 10 capture-exclusion overlay initialization, the MIT x64 ABI declarations from [NvFBC Relay](https://github.com/garrettsparks/nvfbc-relay), System32-only DLL loading, documented NvFBC CreateEx first, legacy key retry without undocumented game PID block, capture-status/error reporting, no WGC/DXGI capture fallback. **NvFBC captures the selected display, not a specific game's PID**.

Use [AppVeyor](https://ci.appveyor.com/project/lumizabergja/wgcextrapolate) to build this isolated branch. If successful, its Artifacts contain FG-V15-Community-NvFBC-Windows-x64.zip.

Community [keylase NvFBC wrapper](https://github.com/keylase/nvidia-patch/blob/master/win/nvfbcwrp/README.md) may enable GeForce but system driver modification is not automated here. No guarantee it works for RTX 5060 Ti. Windows NvFBC has been deprecated since Capture SDK 7.1; do not assume a passing compiler build proves it works.
