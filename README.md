# V15 Looking Glass DXGI Freeze Fix v2

Experimental Windows D3D11 DXGI Desktop Duplication -> NVIDIA optical flow -> original V15 shader -> DXGI flip presenter. This is a **freeze-diagnostic fix**, not yet GPU-verified.

Changes: set WCA_EXCLUDED_FROM_DDA (compositor attribute 24) in addition to WDA to prevent DXGI presenter recursive capture; limit the GPU-copy fence polling to 60 ms and the capture-slot wait to 16 ms. Normal frames do not incur a 60-ms delay. Errors terminate the session with a reason; no capture API fallback.

The original V15 shader and optical flow settings remain unchanged. SHA256 of verified patched main.cpp: `a33b0dcc3dfa42461433bdadf4528b59387c44f434396f8228d45a954404c3c2`.

CI builds from original V15 v16 via `prepare_source.py` then `patch_freeze.py` and publishes `FG-V15-LookingGlass-DXGI-FreezeFix-Windows-x64.zip`, **only after successful compilation**.

If the new app reports exclusion failure, that Windows composition feature may be unavailable to the program; if it reports GPU copy stalled, the capture thread cannot make GPU progress. No claim of measured latency improvements. Working WGC main and v1 DXGI branch remain unchanged.

Microsoft documents [WCA_EXCLUDED_FROM_DDA](https://learn.microsoft.com/en-us/windows/win32/dwm/windowcompositionattrib) for Desktop Duplication.
