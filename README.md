# FG D3DKMT experimental / DXGI fallback, 320 Hz

Compiled from the exact C++ source in FG-D3DKMT-320-Experimental-Source.zip, with matching vendor headers, icon and Build.cmd. Native D3DKMT acquisition is experimental and not confirmed; the worker falls back to DXGI if validation fails. The title shows the active backend. Test on a normal desktop application first. Frame Warp is optional and requires the original FrameWarpRuntime directory from the supplied LateInput package beside FG.exe; third-party runtime DLLs are not redistributed here.
