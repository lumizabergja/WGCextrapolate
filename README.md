# Looking Glass DXGI capture — Start / Stop (no selection)

This is the proven same-PC **Looking Glass-style DXGI Desktop Duplication fullscreen click-through preview**, now with an ordinary compact **Start** and **Stop** window. No window picker.

**Start** launches fullscreen capture on the primary monitor using the same D3D11 output selection, GPU-only CopyResource and preview shader as the previous working v2. **Stop** releases the D3D11 resources and closes only the fullscreen overlay; the small control window remains open and can start capture again.

**Ctrl + Alt + F8** stops capture and restores the controller if minimized. Close the controller window to exit entirely. Minimize to taskbar while gaming; it remains interactive when restored.

The controller and overlay both request WDA_EXCLUDEFROMCAPTURE to avoid DXGI capture-of-self feedback. The overlay is topmost, borderless, click-through and nonactivating. Start/Stop controller appears above the overlay when visible.

No NvOFA, no frame generation, no WGC, no NvFBC, no frame pacing modifications. The `DxgiCaptureOnly` capture engine source matches the prior working v2 exactly.

## Build
AppVeyor builds `lookingglass-capture-start-stop` and publishes `FG-LookingGlass-StartStop-Windows-x64.zip` after successful compilation.

To build locally from this branch: run `python prepare_source.py`, then `Build.cmd` in a Visual Studio x64 Developer Command Prompt. For convenience, the separately provided source ZIP contains the full `src/main.cpp` and can be built with `Build.cmd` without Python. A compiled EXE requires no Python.

Compiled and actual click-through behavior on the user's machine must be tested before calling the variant stable. An optional existing `--monitor=N` argument is retained but no monitor/window selection is shown in the UI.
