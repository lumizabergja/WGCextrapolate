# FG Mask 320 IddCx integrated experimental build

The FG.exe app retains Mask 320's original DXGI mode, NvOFA 4x4 FAST, extrapolation and center mask. The new IddCx checkbox uses a GPU-only, one-slot keyed-mutex mailbox from the included UMDF virtual display driver. No CPU pixel readback, injection or replacement of the NVIDIA driver. Two GPU copies; no claim of zero-copy or verified latency. Frame timing is measured at receipt. Only one physical primary output and one local-console virtual display are supported in this test. No HDR; BGRA8 only.

## Driver requirement
Read Signing-Status.txt. This package is experimental, not Microsoft-production-signed. A self-signed development certificate is NOT a production signature. Do not alter Secure Boot, test signing, Memory Integrity or other system security to make it load on a gaming PC. Use a correctly authorized driver test system or obtain a properly trusted driver package. Install-Driver.ps1 only stages a package already trusted by the OS, and does not import certificates or change boot configuration.

## Test once the driver is correctly installed
1. Launch IddSampleApp.exe (administrator if required). Leave the window running. Windows creates the FG virtual display. Select Extend these displays, keep your physical monitor primary. Set the virtual display to 1920x1080 / 320 Hz, if supported; use SDR.
2. Put a fullscreen borderless animated test window on the FG virtual monitor. Do not duplicate displays. Keep the entire source client on that display. Never use the virtual display for private content during an opted-in capture.
3. Run FG.exe as administrator if required for the Global shared resource; select that window, enable IddCx: virtual to primary, choose Dynamic 320, and Start. Input stays on the selected game; video is shown on the physical primary monitor. The actual virtual display must be the one created by the included driver; selecting another non-primary monitor is unsupported. No fallback is silently used if IddCx fails.
4. If no GPU frames are received in 5 seconds, FG shows an error. Send the exact message. When testing is finished, Stop FG then press x in the launcher to remove the virtual device. Original DXGI mode works with checkbox off, without the driver.

The bridge only writes to the app-created named GPU texture during an explicitly started session. It uses a DACL granting the current user, SYSTEM, and LocalService access; local console, same GPU required. If the resource name is held by the previous driver instance, close/relaunch the virtual device before restarting FG. A successful compile does not establish working IddCx capture or latency.
