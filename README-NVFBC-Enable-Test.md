# Experimental NvFBC Enable API Test (Windows x64)

This standalone program uses the legacy NVIDIA Capture SDK's
`NvFBC_Enable(NVFBC_STATE_ENABLE)` entry point.

**Warning:** This call can reset/reinitialize the graphics driver, temporarily
blank the screen, disrupt display sessions or running apps, and may fail entirely
on Windows 11 or current GeForce drivers. It cannot guarantee NvFBC support.

1. Save work and close CS2, other games, OBS and video/remote desktop tools.
2. Run `NvFBC-Enable-Test.exe` as Administrator.
3. Review the initial `CapturePossible` status.
4. Type exactly `ENABLE` to allow one driver API call, or press Enter to cancel.
5. Copy the console output or the generated `NvFBC-Enable-Result.txt`.

No source changes are made to FG V15 Clean. The utility does not write Windows
registry entries or replace DLLs; however the NVIDIA API may change driver
capture state.

A result of `0` is success from the enable API. The decisive check is whether
`CapturePossible` then becomes `1`. Any other result should be reported rather
than patched around.
