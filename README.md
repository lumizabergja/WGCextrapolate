# FG V15 CUDA Vulkan — native Latewarp compile experiment

This branch compiles the existing Reflex On + Boost Vulkan app and separately syntax-checks the experimental native NGX adapter against public NVIDIA DLSS SDK headers. The real-frame routing guard is preserved. The mathematical camera/depth reference is tested offline. **Native Reflex 2 is NOT connected to the live presenter**, and no native Frame Warp executable is produced. This is a Windows build/compile test, not a GPU validation. The NVIDIA DLLs supplied by the user are excluded from this public branch. The Windows artifact will contain FG.exe, shaders, and this notice only. Test in an offline environment; actual NVIDIA driver support, capture compatibility, and latency are unverified.

Source reconstruction checks the SHA-256 of the original CUDA/Vulkan archive before adding the real-frame routing modifications and adapter from the local kit.
