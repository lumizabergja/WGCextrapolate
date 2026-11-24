#pragma once
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <utility>
#include <cstring>

// Experimental private Windows desktop-duplication transport. Only uses an
// adapter and NT shared surfaces owned by this process. The kernel handshake
// is not publicly specified: the key 1 -> 0 exchange is a hypothesis, not an
// established D3DKMT contract. Never present unvalidated startup pixels.
namespace fgnative {
using Microsoft::WRL::ComPtr;
struct Open { HDC dc; UINT adapter; LUID luid; UINT source; };
struct Close { UINT adapter; };
struct Create { UINT adapter,source,count,required; HANDLE surfaces[3]; UINT flags; };
struct Destroy { UINT adapter,source; BOOL all; };
struct Info { UINT adapter,source; LARGE_INTEGER present,mouse; UINT accumulated; BOOL coalesced,masked; POINT pointer; BOOL visible; UINT metadata,shape; };
struct Release { UINT adapter,source,nextMutex; };
static_assert(sizeof(Open)==24 && sizeof(Create)==48 && sizeof(Info)==56 && sizeof(Release)==12);
template<class T> using Call = LONG (WINAPI*)(T*);
struct Capture {
    Call<Close> close{}; Call<Create> create{}; Call<Destroy> destroy{};
    Call<Info> get{}; Call<Release> release{};
    Open adapter{}; bool opened{},created{};
    ComPtr<ID3D11Texture2D> tex,stage;
    ComPtr<IDXGIKeyedMutex> mutex;
    HANDLE shared{};
    UINT w{},h{}; LONGLONG last{};
    ULONGLONG began{},lastGood{};
    bool verified{}; bool sampled{};
    std::vector<uint32_t> first;
    static constexpr UINT sampleSize=64;
    static constexpr uint32_t marker=0xFFAC19D3u;
    ~Capture(){Reset();}
    void Reset(){
        if(created){Destroy d{adapter.adapter,adapter.source,FALSE};destroy(&d);created=false;}
        if(shared){CloseHandle(shared);shared=nullptr;}
        mutex.Reset();tex.Reset();stage.Reset();
        if(opened){Close c{adapter.adapter};close(&c);opened=false;}
        adapter={};verified=false;sampled=false;last=0;first.clear();
    }
    bool Init(ID3D11Device* device, IDXGIAdapter* dxgi, const DXGI_OUTPUT_DESC& out){
        Reset();
        const HMODULE mod=GetModuleHandleW(L"gdi32.dll");if(!mod)return false;
        auto open=reinterpret_cast<Call<Open>>(GetProcAddress(mod,"D3DKMTOpenAdapterFromHdc"));
        close=reinterpret_cast<Call<Close>>(GetProcAddress(mod,"D3DKMTCloseAdapter"));
        create=reinterpret_cast<Call<Create>>(GetProcAddress(mod,"D3DKMTCreateOutputDupl"));
        destroy=reinterpret_cast<Call<Destroy>>(GetProcAddress(mod,"D3DKMTDestroyOutputDupl"));
        get=reinterpret_cast<Call<Info>>(GetProcAddress(mod,"D3DKMTOutputDuplGetFrameInfo"));
        release=reinterpret_cast<Call<Release>>(GetProcAddress(mod,"D3DKMTOutputDuplReleaseFrame"));
        if(!open||!close||!create||!destroy||!get||!release)return false;
        HDC dc=CreateDCW(L"DISPLAY",out.DeviceName,nullptr,nullptr);if(!dc)return false;
        adapter.dc=dc;const LONG os=open(&adapter);DeleteDC(dc);adapter.dc=nullptr;
        if(os!=0)return false;opened=true;
        DXGI_ADAPTER_DESC ad{};if(FAILED(dxgi->GetDesc(&ad))||ad.AdapterLuid.LowPart!=adapter.luid.LowPart||ad.AdapterLuid.HighPart!=adapter.luid.HighPart)return false;
        Create pre{};pre.adapter=adapter.adapter;pre.source=adapter.source;
        if(create(&pre)!=0||pre.required!=1)return false; // only tested one-mutex layout
        w=UINT(out.DesktopCoordinates.right-out.DesktopCoordinates.left);h=UINT(out.DesktopCoordinates.bottom-out.DesktopCoordinates.top);
        if(w<sampleSize||h<sampleSize||w>8192||h>8192)return false;
        D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        std::vector<uint32_t> initial(size_t(w)*h,marker);
        D3D11_SUBRESOURCE_DATA data{initial.data(),w*4,0};
        if(FAILED(device->CreateTexture2D(&td,&data,&tex))||FAILED(tex.As(&mutex)))return false;
        ComPtr<IDXGIResource1> res;if(FAILED(tex.As(&res)))return false;
        if(FAILED(res->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&shared)))return false;
        td.Width=td.Height=sampleSize;td.BindFlags=td.MiscFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateTexture2D(&td,nullptr,&stage)))return false;
        Create c{};c.adapter=adapter.adapter;c.source=adapter.source;c.count=1;c.surfaces[0]=shared;
        if(create(&c)!=0)return false;created=true;began=lastGood=0;return true;
    }
    enum class Result { None, Frame, Reject };
    Result NoFrame(){return GetTickCount64()-lastGood>(verified?1200u:750u)?Result::Reject:Result::None;}
    Result Grab(ID3D11DeviceContext* ctx,ID3D11Texture2D* dest,const D3D11_BOX& region,LARGE_INTEGER& stamp){
        if(!created)return Result::Reject;
        if(!began)began=lastGood=GetTickCount64();
        // Nonblocking only. The experimental producer is expected to signal 1.
        // Never acquire key 0 and mistake our own initial allocation for a frame.
        HRESULT sync=mutex->AcquireSync(1,0);
        if(sync==WAIT_TIMEOUT||sync==DXGI_ERROR_WAIT_TIMEOUT)return NoFrame();
        if(sync!=S_OK)return Result::Reject;
        Info f{};f.adapter=adapter.adapter;f.source=adapter.source;
        LONG status=get(&f);
        bool fresh=status==0&&f.present.QuadPart>last&&f.accumulated>0&&!f.masked;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        fresh=fresh&&f.present.QuadPart<=now.QuadPart;
        bool accepted=fresh;
        if(fresh&&!verified){
            // Startup-only readback: require two distinct non-marker samples.
            // A positive metadata call by itself is NOT evidence of pixel capture.
            const UINT x=region.left+(region.right-region.left-sampleSize)/2;
            const UINT y=region.top+(region.bottom-region.top-sampleSize)/2;
            D3D11_BOX sample{x,y,0,x+sampleSize,y+sampleSize,1};
            ctx->CopySubresourceRegion(stage.Get(),0,0,0,0,tex.Get(),0,&sample);
            D3D11_MAPPED_SUBRESOURCE map{};
            HRESULT hr=ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map);
            accepted=false;
            if(SUCCEEDED(hr)){
                std::vector<uint32_t> pixels(sampleSize*sampleSize);
                for(UINT r=0;r<sampleSize;r++)std::memcpy(pixels.data()+r*sampleSize,static_cast<const char*>(map.pData)+size_t(r)*map.RowPitch,sampleSize*4);
                ctx->Unmap(stage.Get(),0);
                const bool changed=std::any_of(pixels.begin(),pixels.end(),[](uint32_t p){return p!=marker;});
                if(changed){if(sampled&&pixels!=first){verified=true;accepted=true;}else if(!sampled){first=std::move(pixels);sampled=true;}}
            }
        }
        if(accepted)ctx->CopySubresourceRegion(dest,0,0,0,0,tex.Get(),0,&region);
        if(fresh)last=f.present.QuadPart;
        // Speculative ownership handoff; abandon the backend on any failure.
        const HRESULT unlock=mutex->ReleaseSync(0);
        if(unlock!=S_OK)return Result::Reject;
        if(status==0){Release r{adapter.adapter,adapter.source,0};if(release(&r)!=0)return Result::Reject;}
        else if(static_cast<uint32_t>(status)!=0xC0000225u)return Result::Reject;
        if(f.masked)return Result::Reject;
        if(accepted){stamp=f.present;lastGood=GetTickCount64();return Result::Frame;}
        return NoFrame();
    }
};
} // namespace fgnative
