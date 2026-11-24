#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <array>
#include <utility>
namespace v124 {
using FactoryFn=HRESULT(WINAPI*)(REFIID,void**);
using DeviceFn=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
void Load(FactoryFn&,DeviceFn&);
void Bind(ID3D12Device*);
// Native GPU resources and third-party CUDA interop must not receive SL proxies.
void NativeInterface(IUnknown*,void**);
void RequireSwapchainProxy(IDXGISwapChain*);
extern void (*stageCallback)(const wchar_t*);
void Stage(const wchar_t*) noexcept;
const wchar_t* LastStage() noexcept;
void CheckAdapter(IDXGIAdapter1*);
void Enable();
void Disable() noexcept;
void Shutdown() noexcept;
void InitializePlanes(ID3D12Device*,ID3D12CommandQueue*,UINT,UINT,UINT,UINT,UINT,UINT);
void ReleasePlanes() noexcept;
// Source pose and target pose must be distinct. All camera values originate
// in measured mouse counts; the game pose is an external timestamp estimate.
struct Camera {
    float yaw{},pitch{},lateYaw{},latePitch{},verticalFov{60},horizontalFov{},aspect{1.7777778f};
};
struct Frame {
    ID3D12Resource* back{};
    ID3D12Resource* mask{};
    ID3D12Resource* motion{};
    UINT index{},sourceWidth{},sourceHeight{};
    Camera camera;
};
// Records only the resource preparation, then supplies frame tags and the
// actual next-camera token. The signed 2.7 plugin performs the warp in its
// own swapchain Present hook, not in a host NGX/custom-shader fallback.
void Prepare(const Frame&);
void LatchCamera(const Camera&);
void PresentEnd();
void ResetCameraHistory() noexcept;
}
