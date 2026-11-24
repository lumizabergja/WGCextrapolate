#pragma once
#include <d3d11_1.h>
#include <string>
// Only the opt-in Mask 320 app creates the named texture. No pixels are written to disk.
class FGBridge {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    ULONGLONG lastAttempt{},lastWrite{};
    DWORD session=0xffffffffu;
    void Reset(){mutex.Reset();texture.Reset();lastWrite=0;}
public:
    void Send(ID3D11Device* device,ID3D11DeviceContext* ctx,IDXGIResource* source){
        if(!source)return;
        const DWORD active=WTSGetActiveConsoleSessionId();
        if(active!=session){Reset();session=active;lastAttempt=0;}
        if(active==0xffffffffu)return;
        const ULONGLONG now=GetTickCount64();
        if(texture&&now-lastWrite>500){Reset();lastAttempt=0;}
        if(!texture){
            if(lastAttempt&&now-lastAttempt<100)return;
            lastAttempt=now;
            Microsoft::WRL::ComPtr<ID3D11Device1> dev1;
            if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dev1))))return;
            const std::wstring name=L"Global\\FGIddBridgeV1-"+std::to_wstring(session);
            if(FAILED(dev1->OpenSharedResourceByName(name.c_str(),DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,
                    IID_PPV_ARGS(&texture))))return;
            if(FAILED(texture.As(&mutex))){Reset();return;}
            lastWrite=now;
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> input;
        if(FAILED(source->QueryInterface(IID_PPV_ARGS(&input))))return;
        D3D11_TEXTURE2D_DESC a{},b{};input->GetDesc(&a);texture->GetDesc(&b);
        if(a.Width!=b.Width||a.Height!=b.Height||a.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||
            a.Format!=b.Format||a.MipLevels!=b.MipLevels||a.ArraySize!=b.ArraySize||
            a.SampleDesc.Count!=1||b.SampleDesc.Count!=1)return;
        const HRESULT hr=mutex->AcquireSync(0,0);
        // AcquireSync documents the raw WAIT_TIMEOUT status (not HRESULT_FROM_WIN32).
#pragma warning(suppress:6221)
        if(hr==static_cast<HRESULT>(WAIT_TIMEOUT))return; // drop update rather than block
        if(hr!=S_OK){Reset();return;}
        ctx->CopyResource(texture.Get(),input.Get());
        ctx->Flush();
        if(mutex->ReleaseSync(1)!=S_OK){Reset();return;}
        lastWrite=now;
    }
};
