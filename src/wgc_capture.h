#pragma once
#include <memory>
#include <stdexcept>
#include <chrono>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wgc=winrt::Windows::Graphics::Capture;
namespace wgd=winrt::Windows::Graphics::DirectX;
namespace wgf=winrt::Windows::Foundation;
struct WgcApartment {
    WgcApartment(){winrt::init_apartment(winrt::apartment_type::multi_threaded);}
    ~WgcApartment(){winrt::uninit_apartment();}
};
struct WgcCapture {
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    std::shared_ptr<void> event;
    winrt::event_token token{};
    bool subscribed{};
    static wgc::GraphicsCaptureItem ItemForWindow(HWND window){
        if(!wgc::GraphicsCaptureSession::IsSupported())throw std::runtime_error("WGC unsupported");
        auto interop=winrt::get_activation_factory<wgc::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem result{nullptr};
        winrt::check_hresult(interop->CreateForWindow(window,winrt::guid_of<wgc::GraphicsCaptureItem>(),winrt::put_abi(result)));
        return result;
    }
    void Open(ID3D11Device* device,const wgc::GraphicsCaptureItem& target,UINT width,UINT height){
        item=target;
        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::check_hresult(device->QueryInterface(dxgi.put()));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
        auto wrapped=inspectable.as<wgd::Direct3D11::IDirect3DDevice>();
        event=std::shared_ptr<void>(CreateEventW(nullptr,FALSE,FALSE,nullptr),[](void* h){if(h)CloseHandle(h);});
        if(!event)throw std::runtime_error("WGC frame event failed");
        pool=wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(wrapped,
            wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,5,{int(width),int(height)});
        token=pool.FrameArrived([signal=event](auto const&,auto const&){SetEvent(signal.get());});
        subscribed=true;
        session=pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        const auto type=L"Windows.Graphics.Capture.GraphicsCaptureSession";
        if(ApiInformation::IsPropertyPresent(type,L"MinUpdateInterval"))
            session.MinUpdateInterval(wgf::TimeSpan{0});
        if(ApiInformation::IsPropertyPresent(type,L"DirtyRegionMode"))
            session.DirtyRegionMode(wgc::GraphicsCaptureDirtyRegionMode::ReportOnly);
        if(ApiInformation::IsPropertyPresent(type,L"IncludeSecondaryWindows"))
            session.IncludeSecondaryWindows(false);
        // Preserve OS capture indicators/consent; do not request border suppression.
        session.StartCapture();
    }
    wgc::Direct3D11CaptureFrame Latest(UINT timeout){
        auto frame=pool.TryGetNextFrame();
        if(!frame&&timeout){
            WaitForSingleObject(event.get(),timeout);
            frame=pool.TryGetNextFrame();
        }
        if(!frame)return nullptr;
        // Free the older lease before draining another frame. No CPU readback,
        // no graphics work in the callback, and bounded mailbox drain.
        for(unsigned i=0;i<4;++i){
            auto newer=pool.TryGetNextFrame();
            if(!newer)break;
            frame.Close();frame=std::move(newer);
        }
        return frame;
    }
    void StopSession() noexcept{
        if(pool&&subscribed){try{pool.FrameArrived(token);}catch(...){}subscribed=false;}
        if(session){try{session.Close();}catch(...){}session=nullptr;}
    }
    ~WgcCapture(){
        StopSession();
        if(pool){try{pool.Close();}catch(...){}}
    }
};
