#pragma once
#include "v124_streamline.h"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <atomic>
#include <stdexcept>
#include <string>

namespace fgwarp {
using Microsoft::WRL::ComPtr;
inline std::atomic<bool> requested{false};
inline std::atomic<long long> mouseX{},mouseY{};
inline void check(HRESULT hr,const char* api){
    if(FAILED(hr))throw std::runtime_error(std::string(api)+" HRESULT "+std::to_string(static_cast<unsigned>(hr)));
}
struct Handle {HANDLE value{};~Handle(){if(value)CloseHandle(value);}};
struct Presenter {
    bool initialized{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> image;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12CommandAllocator> allocator[2];
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 serial{},retired[2]{};
    Handle event;
    v124::Camera camera;
    static constexpr double radiansPerCount=-.022*1.5*3.141592653589793/180.;
    static float angle(long long n){return float(std::remainder(n*radiansPerCount,6.283185307179586));}
    void sourcePose(){camera.yaw=angle(mouseX.load());camera.pitch=angle(mouseY.load());}
    void open(IDXGIAdapter* adapter,ID3D11Device* d11,ID3D11DeviceContext* c11,HWND window,
              DXGI_SWAP_CHAIN_DESC1 desc,UINT srcW,UINT srcH,IDXGISwapChain1** swap,ID3D11Texture2D** target){
        v124::FactoryFn factoryFn{};v124::DeviceFn create{};
        initialized=true;v124::Load(factoryFn,create);
        ComPtr<IDXGIFactory2> factory;check(factoryFn(IID_PPV_ARGS(&factory)),"Warp factory");
        ComPtr<IDXGIAdapter1> adapter1;check(adapter->QueryInterface(IID_PPV_ARGS(&adapter1)),"Warp adapter");
        check(create(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Warp device");
        v124::Bind(device.Get());v124::CheckAdapter(adapter1.Get());
        D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Warp queue");
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};fullscreen.Windowed=TRUE;fullscreen.RefreshRate.Denominator=1;
        check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,&fullscreen,nullptr,swap),"Warp swap chain");
        v124::RequireSwapchainProxy(*swap);check((*swap)->QueryInterface(IID_PPV_ARGS(&chain)),"Warp chain index");
        ComPtr<ID3D12Device> native;v124::NativeInterface(device.Get(),reinterpret_cast<void**>(native.GetAddressOf()));
        ComPtr<ID3D12CommandQueue> nativeQueue;
        v124::NativeInterface(queue.Get(),reinterpret_cast<void**>(nativeQueue.GetAddressOf()));
        queue=std::move(nativeQueue);device=native;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;r.Width=desc.Width;r.Height=desc.Height;
        r.DepthOrArraySize=r.MipLevels=1;r.Format=desc.Format;r.SampleDesc.Count=1;
        r.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        check(native->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&r,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&image)),"Shared warp color");
        check(native->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"Shared warp fence");
        Handle texHandle,fenceHandle;
        check(native->CreateSharedHandle(image.Get(),nullptr,GENERIC_ALL,nullptr,&texHandle.value),"Share color");
        check(native->CreateSharedHandle(fence.Get(),nullptr,GENERIC_ALL,nullptr,&fenceHandle.value),"Share fence");
        ComPtr<ID3D11Device5> dev;check(d11->QueryInterface(IID_PPV_ARGS(&dev)),"D3D11 fence support");
        check(dev->OpenSharedResource1(texHandle.value,IID_PPV_ARGS(target)),"Open warp color");
        check(dev->OpenSharedFence(fenceHandle.value,IID_PPV_ARGS(&fence11)),"Open warp fence");
        check(c11->QueryInterface(IID_PPV_ARGS(&context)),"Warp context");
        for(auto& a:allocator)check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"Warp allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator[0].Get(),nullptr,IID_PPV_ARGS(&list)),"Warp command list");
        check(list->Close(),"Warp list close");event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event.value)throw std::runtime_error("Warp fence event creation failed");
        v124::InitializePlanes(device.Get(),queue.Get(),desc.Width,desc.Height,srcW,srcH,2,UINT(1000.0*srcW/srcH));
        camera.aspect=float(srcW)/srcH;camera.verticalFov=73.74f;
        v124::Enable();
    }
    void wait(UINT64 n){if(n && fence->GetCompletedValue()<n){
        check(fence->SetEventOnCompletion(n,event.value),"Warp fence event");
        if(WaitForSingleObject(event.value,3000)!=WAIT_OBJECT_0)throw std::runtime_error("Warp GPU timed out");
    }}
    void present(UINT flags,UINT w,UINT h,bool real){
        UINT i=chain->GetCurrentBackBufferIndex();wait(retired[i]);
        check(context->Signal(fence11.Get(),++serial),"Publish DXGI frame");context->Flush();
        check(queue->Wait(fence.Get(),serial),"Wait for DXGI color");
        ComPtr<ID3D12Resource> back;check(chain->GetBuffer(i,IID_PPV_ARGS(&back)),"Warp back buffer");
        check(allocator[i]->Reset(),"Warp allocator reset");check(list->Reset(allocator[i].Get(),nullptr),"Warp copy reset");
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={back.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(1,&b);list->CopyResource(back.Get(),image.Get());
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);check(list->Close(),"Warp copy close");
        ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        v124::Frame frame{};frame.back=back.Get();frame.index=i;frame.sourceWidth=w;frame.sourceHeight=h;frame.camera=camera;
        v124::Prepare(frame);
        // The overlay and presenter share a thread. Dispatch mouse reports
        // queued during GPU preparation before sampling the late pose.
        if(real){
            MSG input{};
            for(unsigned i=0;i<32 && PeekMessageW(&input,nullptr,WM_INPUT,WM_INPUT,PM_REMOVE);++i)
                DispatchMessageW(&input);
        }
        camera.lateYaw=real?angle(mouseX.load()):camera.yaw;camera.latePitch=real?angle(mouseY.load()):camera.pitch;
        v124::LatchCamera(camera);check(chain->Present(0,flags),"Native Frame Warp Present");v124::PresentEnd();
        check(queue->Signal(fence.Get(),++serial),"Retire warp frame");retired[i]=serial;
        check(context->Wait(fence11.Get(),serial),"Return shared color");
    }
    ~Presenter(){if(initialized){if(queue&&fence&&event.value){try{if(SUCCEEDED(queue->Signal(fence.Get(),++serial)))wait(serial);}catch(...){}}
        v124::Disable();v124::ReleasePlanes();chain.Reset();list.Reset();for(auto& a:allocator)a.Reset();queue.Reset();device.Reset();v124::Shutdown();}}
};
}
