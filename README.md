# FG WGC 320 — Native D3D12 Frame Warp (experimental)

Windows 11 26H2 target. External WGC HWND capture, 320 Hz Dynamic default, static Off–x6, click-through overlay, maximum DXGI frame latency 1.

Frame Warp OFF: strict native D3D11 zero-intermediate-copy baseline. Frame Warp ON: initialize WGC using D3D11On12 on the native D3D12 graphics queue. Checkout the captured allocation with ID3D11On12Device2::UnwrapUnderlyingResource, register with native D3D12 NvOFA, and render the existing real/extrapolation shaders directly into the D3D12 swapchain. No D3D11 intermediate color render or copy to the swapchain. The existing Streamline 2.7 plugin retains its separate HUD-less color copy and flat-depth/zero-motion model. Real-only late pose correction samples input after obtaining the next camera token, immediately before submitting camera constants and Present. Estimated external pose is not game-integrated Reflex.

Strict capture unwrap and OFA registration: fail visibly if unsupported, no automatic copy fallback. Keep WGC leases checked out until both graphics and OFA fences complete, then unregister and return resources. Native OFA asynchronous, skip stale work; do not queue a fake frame while motion is unfinished. OS/driver internal copies and scanout latency remain. Actual compatibility, latency and Independent Flip require live testing.

Build with Windows SDK 26100+ and MSVC x64. NvOFA D3D12 header is NVIDIA's licensed header from https://github.com/SlimRG/NvidiaOF5 (notice retained). The CI artifact contains FG.exe and README. Place the six original DLLs in FrameWarpRuntime/ alongside FG.exe; no runtime redistribution to public repository.
