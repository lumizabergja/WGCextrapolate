#pragma once
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <utility>
#include <cstring>
#include <cstdio>
#include <string>

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
    void (*stageCallback)(const wchar_t*){};
    std::string detail{"D3DKMT native capture failed"};
    void Step(const wchar_t* name){if(stageCallback)stageCallback(name);}
    bool Fail(const char* op,LONG status=0){
        char b[220];std::snprintf(b,sizeof(b),"%s: 0x%08lX",op,(unsigned long)status);detail=b;return false;
    }
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
        if(created){Step(L"D3DKMTDestroyOutputDupl");Destroy d{adapter.adapter,adapter.source,FALSE};destroy(&d);created=false;}
        if(shared){CloseHandle(shared);shared=nullptr;}
        mutex.Reset();tex.Reset();stage.Reset();
        if(opened){Close c{adapter.adapter};close(&c);opened=false;}
        adapter={};verified=false;sampled=false;last=0;first.clear();
    }
    bool Init(ID3D11Device* device, IDXGIAdapter* dxgi, const DXGI_OUTPUT_DESC& out){
        Reset();
        Step(L"Resolve D3DKMT exports");
        const HMODULE mod=GetModuleHandleW(L"gdi32.dll");if(!mod)return Fail("GetModuleHandle gdi32",GetLastError());
        auto open=reinterpret_cast<Call<Open>>(GetProcAddress(mod,"D3DKMTOpenAdapterFromHdc"));
        close=reinterpret_cast<Call<Close>>(GetProcAddress(mod,"D3DKMTCloseAdapter"));
        create=reinterpret_cast<Call<Create>>(GetProcAddress(mod,"D3DKMTCreateOutputDupl"));
        destroy=reinterpret_cast<Call<Destroy>>(GetProcAddress(mod,"D3DKMTDestroyOutputDupl"));
        get=reinterpret_cast<Call<Info>>(GetProcAddress(mod,"D3DKMTOutputDuplGetFrameInfo"));
        release=reinterpret_cast<Call<Release>>(GetProcAddress(mod,"D3DKMTOutputDuplReleaseFrame"));
        if(!open||!close||!create||!destroy||!get||!release)return Fail("Missing D3DKMT export",ERROR_PROC_NOT_FOUND);
        Step(L"D3DKMTOpenAdapterFromHdc");
        HDC dc=CreateDCW(L"DISPLAY",out.DeviceName,nullptr,nullptr);if(!dc)return Fail("CreateDC",GetLastError());
        adapter.dc=dc;const LONG os=open(&adapter);DeleteDC(dc);adapter.dc=nullptr;
        if(os!=0)return Fail("D3DKMTOpenAdapterFromHdc",os);opened=true;
        DXGI_ADAPTER_DESC ad{};if(FAILED(dxgi->GetDesc(&ad))||ad.AdapterLuid.LowPart!=adapter.luid.LowPart||ad.AdapterLuid.HighPart!=adapter.luid.HighPart)return Fail("Adapter LUID mismatch");
        Create pre{};pre.adapter=adapter.adapter;pre.source=adapter.source;
        Step(L"D3DKMTCreateOutputDupl preflight");
        LONG preStatus=create(&pre);
        if(preStatus!=0)return Fail("D3DKMTCreateOutputDupl preflight",preStatus);
        if(pre.required!=1)return Fail("D3DKMT required mutex count (expected 1)",pre.required);
        w=UINT(out.DesktopCoordinates.right-out.DesktopCoordinates.left);h=UINT(out.DesktopCoordinates.bottom-out.DesktopCoordinates.top);
        if(w<sampleSize||h<sampleSize||w>8192||h>8192)return Fail("Unsupported native surface dimensions");
        D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        std::vector<uint32_t> initial(size_t(w)*h,marker);
        D3D11_SUBRESOURCE_DATA data{initial.data(),w*4,0};
        Step(L"Allocate native shared texture");
        HRESULT hr=device->CreateTexture2D(&td,&data,&tex);
        if(FAILED(hr))return Fail("Create native texture",hr);
        hr=tex.As(&mutex);if(FAILED(hr))return Fail("Query keyed mutex",hr);
        ComPtr<IDXGIResource1> res;hr=tex.As(&res);if(FAILED(hr))return Fail("Query IDXGIResource1",hr);
        hr=res->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&shared);if(FAILED(hr))return Fail("CreateSharedHandle",hr);
        // Validate our own shared surface before handing it to the system.
        // Key zero is the documented initial key. Do not retain ownership.
        Step(L"Shared texture pre-registration AcquireSync(0)");
        hr=mutex->AcquireSync(0,0);
        if(hr!=S_OK)return Fail("Pre-registration AcquireSync key 0",hr);
        Step(L"Shared texture pre-registration ReleaseSync(0)");
        hr=mutex->ReleaseSync(0);
        if(hr!=S_OK)return Fail("Pre-registration ReleaseSync key 0",hr);
        td.Width=td.Height=sampleSize;td.BindFlags=td.MiscFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr=device->CreateTexture2D(&td,nullptr,&stage);if(FAILED(hr))return Fail("Create startup validation texture",hr);
        Create c{};c.adapter=adapter.adapter;c.source=adapter.source;c.count=1;c.surfaces[0]=shared;
        Step(L"D3DKMTCreateOutputDupl session");
        LONG st=create(&c);if(st!=0)return Fail("D3DKMTCreateOutputDupl session",st);
        created=true;began=lastGood=0;detail="D3DKMT waiting for first valid frame";return true;
    }
    enum class Result { None, Frame, Reject, Abandoned };
    Result Reject(const char* op,LONG status=0){Fail(op,status);return Result::Reject;}
    Result NoFrame(){if(GetTickCount64()-lastGood>(verified?3000u:4000u)){detail="Native frame timeout: "+detail;return Result::Reject;}return Result::None;}
    Result Grab(ID3D11DeviceContext* ctx,ID3D11Texture2D* dest,const D3D11_BOX& region,LARGE_INTEGER& stamp){
        if(!created)return Reject("D3DKMT session not created");
        if(!began)began=lastGood=GetTickCount64();
        // Nonblocking only. The experimental producer is expected to signal 1.
        // Never acquire key 0 and mistake our own initial allocation for a frame.
        Step(L"Native keyed mutex AcquireSync(1,0)");
        HRESULT sync=mutex->AcquireSync(1,0);
        if(sync==WAIT_TIMEOUT||sync==DXGI_ERROR_WAIT_TIMEOUT){Fail("AcquireSync key 1 timeout",sync);return NoFrame();}
        if(sync==WAIT_ABANDONED){
            Fail("Post-registration AcquireSync key 1 WAIT_ABANDONED (pre-registration key 0 passed)",sync);
            return Result::Abandoned;
        }
        if(sync!=S_OK)return Reject("AcquireSync key 1",sync);
        Info f{};f.adapter=adapter.adapter;f.source=adapter.source;
        Step(L"D3DKMTOutputDuplGetFrameInfo");
        LONG status=get(&f);
        char meta[220];std::snprintf(meta,sizeof(meta),"GetFrameInfo=0x%08lX present=%lld accumulated=%u masked=%d",(unsigned long)status,(long long)f.present.QuadPart,f.accumulated,int(f.masked));detail=meta;
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
            Step(L"Startup GPU pixel validation Map");
            D3D11_MAPPED_SUBRESOURCE map{};
            HRESULT hr=ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map);
            accepted=false;
            if(SUCCEEDED(hr)){
                std::vector<uint32_t> pixels(sampleSize*sampleSize);
                for(UINT r=0;r<sampleSize;r++)std::memcpy(pixels.data()+r*sampleSize,static_cast<const char*>(map.pData)+size_t(r)*map.RowPitch,sampleSize*4);
                ctx->Unmap(stage.Get(),0);
                const bool changed=std::any_of(pixels.begin(),pixels.end(),[](uint32_t p){return p!=marker;});
                if(changed){if(sampled&&pixels!=first){verified=true;accepted=true;}else if(!sampled){first=std::move(pixels);sampled=true;}}
                if(!accepted)detail=changed?"Startup validation: waiting for second distinct pixel sample":"Startup validation: texture still contains marker";
            }else Fail("Startup validation Map",hr);
        }
        if(accepted)ctx->CopySubresourceRegion(dest,0,0,0,0,tex.Get(),0,&region);
        if(fresh)last=f.present.QuadPart;
        // Speculative ownership handoff; abandon the backend on any failure.
        Step(L"Native keyed mutex ReleaseSync(0)");
        const HRESULT unlock=mutex->ReleaseSync(0);
        if(unlock!=S_OK)return Reject("ReleaseSync key 0",unlock);
        if(status==0){Step(L"D3DKMTOutputDuplReleaseFrame");Release r{adapter.adapter,adapter.source,0};LONG rs=release(&r);if(rs!=0)return Reject("D3DKMTOutputDuplReleaseFrame",rs);}
        else if(static_cast<uint32_t>(status)!=0xC0000225u)return Reject("D3DKMTOutputDuplGetFrameInfo",status);
        if(f.masked)return Reject("Protected content reported; capture stopped");
        if(accepted){stamp=f.present;lastGood=GetTickCount64();return Result::Frame;}
        return NoFrame();
    }
};
} // namespace fgnative
