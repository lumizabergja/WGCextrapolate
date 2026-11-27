#pragma once
enum class FrameKind { Real, Generated, Repeat };
constexpr FrameKind ClassifyFrame(bool newCapture, float horizon) noexcept {
    return newCapture ? FrameKind::Real : (horizon > 0.0f ? FrameKind::Generated : FrameKind::Repeat);
}
constexpr bool ShouldRecordNativeLatewarp(FrameKind kind, bool backendReady) noexcept {
    return backendReady && kind == FrameKind::Real;
}
#if defined(FG_ENABLE_NATIVE_LATEWARP)
#error Native NGX Frame Warp SDK and validated Vulkan backend are required; this package only implements frame routing.
#endif
