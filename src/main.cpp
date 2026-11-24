// FG 320: native Windows Graphics Capture (Windows 11 24H2/26H2).
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>
#include <array>
#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <cmath>
#include <cwchar>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <nvOpticalFlowD3D11.h>
#include "warp_presenter.h"
#include "wgc_capture.h"

using Microsoft::WRL::ComPtr;
namespace {
constexpr int kNumCapture = 4; // at most two active, two retiring
constexpr int kOpticalFlowGrid = 4; // This app has a fixed NvOFA FAST 4x4 path.
constexpr UINT kIconId = 101;
constexpr UINT kEmergencyHotkey = 0x5240;
constexpr int kTarget = 101, kRefresh = 102, kMultiplier = 103, kStart = 104, kStop = 105,
    kDynamicTarget = 107, kCenterRadius = 108, kCenterFeather = 109;
constexpr UINT WM_FG_STATUS = WM_APP+77, WM_FG_STOPPED = WM_APP+78;
HWND g_control{},g_targets{},g_multi{},g_start{},g_stop{},g_overlay{},g_dynamicTarget{};
HWND g_centerRadiusEdit{},g_centerFeatherEdit{};
HWND g_refresh{};
HFONT g_font{},g_fontBold{},g_fontSmall{},g_fontHero{};
HBRUSH g_bgBrush{},g_fieldBrush{},g_panelBrush{};
const COLORREF kBackground=RGB(10,14,22),kPanel=RGB(20,27,38),kField=RGB(26,36,49),kText=RGB(233,244,250),kMuted=RGB(145,160,173),kAccent=RGB(61,226,220);
std::atomic<bool> g_running{false};
// Separate user-requested Stop from a transient invalidated capture session.
// The latter ends one GPU session before the supervisor restarts it.
std::atomic<bool> g_userStopRequested{false};
// The presenter may temporarily stop during WGC capture recovery. Only the outer
// Engine() completion marks the launcher as ready for a fresh Start.
std::atomic<bool> g_engineFinished{true};
std::atomic<bool> g_lastGameExited{false};
constexpr UINT_PTR kLauncherLifecycleTimer = 0xF615;
// 0=Dynamic (target refresh), 1=Off, 2..6=static x2..x6.
std::atomic<int> g_fgMode{0};
std::atomic<int> g_targetHz{320};
// Center-only protection; zero radius and zero feather disable protection.
std::atomic<unsigned> g_centerRadiusPx{0},g_centerFeatherPx{0};
std::thread g_worker;
std::mutex g_statusLock;
std::wstring g_statusText;
struct Game {HWND window;DWORD pid;std::wstring name;};
std::vector<Game> g_games;

struct SessionState{
    HWND game;DWORD pid;LONG warp;volatile LONG stop,mode,target,radius,feather;
    wchar_t error[512],stage[96];
};
SessionState* session{};
void Status(const std::wstring& s) {
    if(session && (s.rfind(L"FG error:",0)==0||s.rfind(L"Source error:",0)==0))lstrcpynW(session->error,s.c_str(),512);
    {std::lock_guard<std::mutex> l(g_statusLock);g_statusText=s;}
    if(g_control) PostMessageW(g_control,WM_FG_STATUS,0,0);
}
std::runtime_error Error(const std::string& what,HRESULT hr=0) {
    char b[128];std::snprintf(b,sizeof(b),"%s (0x%08lX)",what.c_str(),(unsigned long)hr);
    return std::runtime_error(b);
}
void Check(HRESULT hr,const char* what) {if(FAILED(hr)) throw Error(what,hr);}
void NvCheck(NV_OF_STATUS s,const char* what) {if(s!=NV_OF_SUCCESS)throw Error(what,static_cast<HRESULT>(s));}
struct DisplayChanged {};
std::wstring Wide(const std::string& s) {int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);if(n<1)return L"Error";std::wstring w(size_t(n),L'\0');MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,w.data(),n);w.pop_back();return w;}
BOOL CALLBACK FindWindows(HWND h,LPARAM) {
    if(!IsWindowVisible(h)||h==g_control||h==g_overlay||GetWindow(h,GW_OWNER))return TRUE;
    wchar_t title[256]{};GetWindowTextW(h,title,256);if(!title[0])return TRUE;
    DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(!pid||pid==GetCurrentProcessId())return TRUE;
    RECT r{};GetWindowRect(h,&r);if(r.right-r.left<320||r.bottom-r.top<200)return TRUE;
    g_games.push_back({h,pid,title});return TRUE;
}
void RefreshGames(){
    SendMessageW(g_targets,CB_RESETCONTENT,0,0);g_games.clear();EnumWindows(FindWindows,0);
    for(const auto& g:g_games)SendMessageW(g_targets,CB_ADDSTRING,0,(LPARAM)g.name.c_str());
    if(!g_games.empty())SendMessageW(g_targets,CB_SETCURSEL,0,0);
}

struct OverlayWindow {
    HWND hwnd{};
    UINT width{},height{};
    LONG originX{},originY{};
    bool visible{};
    // Track mode changes when an exclusive 4:3 game regains focus.
    // The HWND must occupy the current logical monitor (not the old desktop mode).
    bool SyncToMonitor(HWND target) {
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(MonitorFromWindow(target,MONITOR_DEFAULTTONEAREST),&mi))
            throw Error("GetMonitorInfo during mode switch failed",GetLastError());
        const RECT r=mi.rcMonitor;
        const UINT w=UINT(r.right-r.left),h=UINT(r.bottom-r.top);
        if(w<320||h<200)throw Error("Invalid output display mode");
        if(w==width&&h==height&&r.left==originX&&r.top==originY)return false;
        if(!SetWindowPos(hwnd,HWND_TOPMOST,r.left,r.top,int(w),int(h),SWP_NOACTIVATE))
            throw Error("Resize click-through presenter failed",GetLastError());
        width=w;height=h;originX=r.left;originY=r.top;
        return true;
    }
    void SetVisible(bool active) {
        if(!hwnd || active==visible) return;
        // Never activate or focus our click-through presenter on Alt+Tab.
        ShowWindow(hwnd,active?SW_SHOWNOACTIVATE:SW_HIDE);
        if(active) SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        visible=active;
    }
    void Init(HWND target){
        // Fullscreen non-layered topmost HWNDs cannot reliably pass menu clicks
        // to CS2. Use a permanently layered, nonactivating click-through HWND.
        // The source game stays in control of keyboard/mouse input.
        HMONITOR monitor=MonitorFromWindow(target,MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(monitor,&mi))throw Error("GetMonitorInfo failed",GetLastError());
        RECT r=mi.rcMonitor;width=r.right-r.left;height=r.bottom-r.top;
        originX=r.left;originY=r.top;
        const DWORD normalEx=WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_LAYERED|WS_EX_TOOLWINDOW;
        hwnd=CreateWindowExW(normalEx,
            L"FGDXGIOverlay",L"FG",WS_POPUP,r.left,r.top,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!hwnd)throw Error("Create presenter window failed",GetLastError());
        // Full-opacity layered click-through overlay: input reaches CS2 underneath.
        if(!SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA))
            throw Error("SetLayeredWindowAttributes failed",GetLastError());
        // Fail closed: without capture exclusion the presenter can recursively capture itself.
        if(!SetWindowDisplayAffinity(hwnd,0x00000011))throw Error("WDA_EXCLUDEFROMCAPTURE unsupported",GetLastError());
        // Do not show over the control window; engine reveals it only when game has focus.
        ShowWindow(hwnd,SW_HIDE);
        visible=false;
        g_overlay=hwnd;
    }
    ~OverlayWindow(){if(hwnd)DestroyWindow(hwnd);g_overlay=nullptr;}
};

struct DxDevices {
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDXGISwapChain2> lowLatencySwap;
    HANDLE frameReady{}; // Closed before releasing the swapchain.
    bool allowTearing{};
    LARGE_INTEGER captureFrequency{},lastCaptureStamp{};
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    // A WGC frame lease owns the original surface until GPU retirement.
    wgc::Direct3D11CaptureFrame leases[kNumCapture]{nullptr,nullptr,nullptr,nullptr};
    ComPtr<ID3D11Texture2D> history[kNumCapture];
    ComPtr<ID3D11DeviceContext4> fenceContext;
    ComPtr<ID3D11Fence> retirementFence;
    UINT64 nextFence{};
    UINT64 retiring[kNumCapture]{};
    HANDLE retireEvent{};
    ComPtr<ID3D11ShaderResourceView> frameSRV[kNumCapture];
    UINT srcW{},srcH{},screenW{},screenH{};
    ComPtr<IDXGIAdapter> matchingAdapter;
    WgcCapture capture;
    DXGI_OUTPUT_DESC desktop{};
    ~DxDevices(){
        // Flow11 has already retired GPU work and unregistered OFA resources.
        capture.StopSession();
        for(int i=0;i<kNumCapture;++i)ReleaseLease(i);
        if(retireEvent)CloseHandle(retireEvent);
        if(frameReady)CloseHandle(frameReady);
    }
    void ReleaseLease(int i) noexcept{
        frameSRV[i].Reset();history[i].Reset();retiring[i]=0;
        if(leases[i]){try{leases[i].Close();}catch(...){}leases[i]=nullptr;}
    }
    int FreeSlot() const{
        for(int i=0;i<kNumCapture;++i)if(!leases[i])return i;
        return -1;
    }
    void RetireExcept(int current,int previous){
        bool pending=false;
        for(int i=0;i<kNumCapture;++i)
            if(leases[i]&&!retiring[i]&&i!=current&&i!=previous)pending=true;
        if(!pending)return;
        const UINT64 value=++nextFence;
        Check(fenceContext->Signal(retirementFence.Get(),value),"Signal WGC retirement fence");
        for(int i=0;i<kNumCapture;++i)
            if(leases[i]&&!retiring[i]&&i!=current&&i!=previous)retiring[i]=value;
        // OFA may have queued work after the previous Present; submit once per
        // retired batch, without waiting on the render thread.
        ctx11->Flush();
    }
    UINT64 Completed() const {
        const UINT64 v=retirementFence->GetCompletedValue();
        if(v==UINT64_MAX)throw Error("WGC retirement device removed",dev11->GetDeviceRemovedReason());
        return v;
    }
    void DrainForShutdown() noexcept{
        capture.StopSession(); // no producer can reuse surfaces during teardown
        if(!fenceContext||!retirementFence||!retireEvent)return;
        const UINT64 value=++nextFence;
        if(FAILED(fenceContext->Signal(retirementFence.Get(),value)))return;
        ctx11->Flush();
        if(retirementFence->GetCompletedValue()<value &&
           SUCCEEDED(retirementFence->SetEventOnCompletion(value,retireEvent)))
            WaitForSingleObject(retireEvent,2000);
    }
    void Init(HWND output,const wgc::GraphicsCaptureItem& item,UINT w,UINT h,UINT outputW,UINT outputH){
        srcW=w;srcH=h;screenW=outputW;screenH=outputH;
        HMONITOR monitor=MonitorFromWindow(output,MONITOR_DEFAULTTONEAREST);
        ComPtr<IDXGIFactory1> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create DXGI factory");
        ComPtr<IDXGIOutput1> captureOutput;
        for(UINT i=0;!captureOutput;++i){
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT hr=factory->EnumAdapters1(i,&adapter);
            if(hr==DXGI_ERROR_NOT_FOUND)break;
            Check(hr,"Enumerate adapters");
            for(UINT j=0;;++j){
                ComPtr<IDXGIOutput> candidate;
                hr=adapter->EnumOutputs(j,&candidate);
                if(hr==DXGI_ERROR_NOT_FOUND)break;
                Check(hr,"Enumerate outputs");
                Check(candidate->GetDesc(&desktop),"Read output description");
                if(desktop.Monitor!=monitor)continue;
                Check(candidate.As(&captureOutput),"Query monitor output");
                matchingAdapter=adapter;
                break;
            }
        }
        if(!captureOutput)throw Error("Selected monitor unavailable");
        if(desktop.Rotation!=DXGI_MODE_ROTATION_IDENTITY)
            throw Error("Select an unrotated monitor");
        D3D_FEATURE_LEVEL obtained{};
        Check(D3D11CreateDevice(matchingAdapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&dev11,&obtained,&ctx11),"D3D11CreateDevice");
        ComPtr<ID3D11Device5> fenceDevice;
        Check(dev11.As(&fenceDevice),"Zero-copy requires ID3D11Device5");
        Check(ctx11.As(&fenceContext),"Zero-copy requires ID3D11DeviceContext4");
        Check(fenceDevice->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&retirementFence)),"Create WGC retirement fence");
        retireEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!retireEvent)throw Error("Create GPU retire event",GetLastError());
        QueryPerformanceFrequency(&captureFrequency);
        capture.Open(dev11.Get(),item,w,h);
        ComPtr<IDXGIFactory2> f2;Check(matchingAdapter->GetParent(IID_PPV_ARGS(&f2)),"GetParent IDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 sc{};sc.Width=screenW;sc.Height=screenH;
        sc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount=2;sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sc.Scaling=DXGI_SCALING_STRETCH;
        sc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        // DXGI cannot have a literal maximum frame latency of 0: its minimum is 1.
        // Retain the newest source while the display slot is busy and
        // do not submit a new present until DXGI frees a frame slot.
        ComPtr<IDXGIFactory5> factory5;
        if(SUCCEEDED(f2.As(&factory5))){
            BOOL supported=FALSE;
            if(SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &supported,sizeof(supported))) && supported)
                allowTearing=true;
        }
        sc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        Check(f2->CreateSwapChainForHwnd(dev11.Get(),output,&sc,nullptr,nullptr,&swap),"CreateSwapChainForHwnd zero-copy");
        Check(swap.As(&lowLatencySwap),"Query IDXGISwapChain2");
        Check(lowLatencySwap->SetMaximumFrameLatency(1),"Set swapchain max latency 1 (DXGI minimum)");
        frameReady=lowLatencySwap->GetFrameLatencyWaitableObject();
        if(!frameReady)throw Error("Swapchain frame latency waitable object unavailable");
        Check(swap->GetBuffer(0,IID_PPV_ARGS(&backBuffer)),"GetBuffer");
        Check(dev11->CreateRenderTargetView(backBuffer.Get(),nullptr,&rtv),"CreateRenderTargetView");
        // Fullscreen triangle: PS renders input or extrapolated source directly into the swapchain RTV.
        // Port of the uploaded FG-Fast-Crosshair extrapolator.h HLSL; see PROVENANCE.md.
        static const char* shader=R"HLSL(
Texture2D<float4> currentFrame:register(t0);
Texture2D<int2> rawFlow:register(t1);
Texture2D<float4> previousFrame:register(t2);
SamplerState samp:register(s0);
cbuffer Settings:register(b0){float horizon;float2 motionInv;float maxMotion;float2 gridExtent;float gridSize;float _padding1;float centerRadiusPx;float centerFeatherPx;float2 outputSize;};
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;nointerpolation float scene:TEXCOORD1;};
// Use native block centers, including partial cells on odd-sized captures.
float2 flow(float2 uv){
    uint fw,fh;rawFlow.GetDimensions(fw,fh);
    float2 pos=uv/(motionInv*gridSize*gridExtent)*float2(fw,fh)-0.5;
    int2 base=int2(floor(pos));float2 f=frac(pos);
    int2 edge=int2(int(fw)-1,int(fh)-1);
    int2 a=clamp(base,int2(0,0),edge),b=clamp(base+int2(1,0),int2(0,0),edge);
    int2 c=clamp(base+int2(0,1),int2(0,0),edge),d=clamp(base+int2(1,1),int2(0,0),edge);
    float2 v00=float2(rawFlow.Load(int3(a,0)))*(1.0/32.0);
    float2 v10=float2(rawFlow.Load(int3(b,0)))*(1.0/32.0);
    float2 v01=float2(rawFlow.Load(int3(c,0)))*(1.0/32.0);
    float2 v11=float2(rawFlow.Load(int3(d,0)))*(1.0/32.0);
    return lerp(lerp(v00,v10,f.x),lerp(v01,v11,f.x),f.y)*motionInv;
}
float cutSample(float2 uv){float2 q=uv+flow(uv);if(any(q<0)||any(q>1))return 0;return dot(abs(currentFrame.SampleLevel(samp,uv,0).rgb-previousFrame.SampleLevel(samp,q,0).rgb),float3(.333333,.333333,.333333));}
V VS(uint id:SV_VertexID){
    V o;float2 p=float2(id==2?3.0:-1.0,id==1?3.0:-1.0);
    o.p=float4(p,0,1);o.uv=float2((p.x+1)*0.5,1-(p.y+1)*0.5);o.scene=0;
    [branch] if(horizon>0.00001){float e=cutSample(float2(.2,.3))+cutSample(float2(.8,.3))+cutSample(float2(.5,.5))+cutSample(float2(.2,.7))+cutSample(float2(.8,.7));o.scene=smoothstep(.19,.32,e*.2);}
    return o;
}
float centerMask(float2 uv){
    // The center mask affects only predicted pixels, never real captured frames.
    // 0px radius + 0px feather is a full bypass (no center protection).
    if(centerRadiusPx<=0.0 && centerFeatherPx<=0.0)return 1.0;
    float distancePx=length((uv-float2(.5,.5))*outputSize);
    if(centerFeatherPx<=0.0)return distancePx>centerRadiusPx?1.0:0.0;
    return smoothstep(centerRadiusPx,centerRadiusPx+centerFeatherPx,distancePx);
}
float4 PS(V i):SV_Target {
    // NVIDIA's stretched scanout is already scaled into native game-size textures.
    float2 uv=i.uv;
    float4 base=currentFrame.SampleLevel(samp,uv,0);
    [branch] if(horizon<=0.00001 || i.scene>0.9)return base;
    float2 v=flow(uv),q=uv+horizon*v;
    if(any(q<0)||any(q>1))return base;
    float2 mv=flow(q),old=q+mv;
    if(any(old<0)||any(old>1)||length(mv)>maxMotion)return base;
    float4 next=currentFrame.SampleLevel(samp,q,0);
    float3 prev=previousFrame.SampleLevel(samp,old,0).rgb;
    float residual=dot(abs(next.rgb-prev),float3(.333333,.333333,.333333));
    float confidence=(1-smoothstep(.045,.20,residual))*(1-smoothstep(.003,.025,length(mv-v)));
    return lerp(base,next,saturate(confidence*centerMask(uv)));
}
)HLSL";
        ComPtr<ID3DBlob> vsBlob,psBlob,errs;
        HRESULT hr=D3DCompile(shader,strlen(shader),"FG",nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vsBlob,&errs);
        if(FAILED(hr))throw Error(errs?std::string((char*)errs->GetBufferPointer(),errs->GetBufferSize()):"VS compile",hr);
        Check(dev11->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,&vs),"VertexShader");
        errs.Reset();hr=D3DCompile(shader,strlen(shader),"FG",nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&psBlob,&errs);
        if(FAILED(hr))throw Error(errs?std::string((char*)errs->GetBufferPointer(),errs->GetBufferSize()):"PS compile",hr);
        Check(dev11->CreatePixelShader(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),nullptr,&ps),"PixelShader");
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        Check(dev11->CreateSamplerState(&sd,&sampler),"Sampler");
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=48;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(dev11->CreateBuffer(&bd,nullptr,&constants),"Constants");
    }
    bool Grab(HWND game,int slot,UINT timeout,LARGE_INTEGER& timestamp){
        if(!IsWindow(game))return false;
        if(MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST)!=desktop.Monitor)throw DisplayChanged{};
        if(slot<0||leases[slot])return false;
        const auto frame=capture.Latest(timeout);
        if(!frame)return false;
        const auto size=frame.ContentSize();
        if(size.Width!=int(srcW)||size.Height!=int(srcH))throw DisplayChanged{};
        const auto texture=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> source;
        Check(texture->GetInterface(IID_PPV_ARGS(&source)),"WGC texture");
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if(desc.Width!=srcW||desc.Height!=srcH||desc.MipLevels!=1||desc.ArraySize!=1||
            desc.SampleDesc.Count!=1||desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)
            throw Error("Zero-copy rejected: WGC texture layout is incompatible");
        if(!(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE))
            throw Error("Zero-copy rejected: WGC texture has no shader-resource binding");
        const auto ticks=frame.SystemRelativeTime().count();
        timestamp.QuadPart=static_cast<LONGLONG>(static_cast<long double>(ticks)*captureFrequency.QuadPart/10000000.0L);
        if(timestamp.QuadPart<=0)QueryPerformanceCounter(&timestamp);
        if(timestamp.QuadPart<=lastCaptureStamp.QuadPart){frame.Close();return false;}
        // No CopyResource, staging, cross-API sharing or CPU readback.
        ComPtr<ID3D11ShaderResourceView> view;
        Check(dev11->CreateShaderResourceView(source.Get(),nullptr,&view),"Zero-copy WGC SRV");
        history[slot]=std::move(source);
        frameSRV[slot]=std::move(view);
        leases[slot]=frame;
        lastCaptureStamp=timestamp;
        return true;
    }
    // 48-byte constant buffer: motion settings plus center-only protection.
    struct Params{
        float horizon, invW, invH, maxMotion;
        float flowW, flowH, grid, padding1;
        float centerRadiusPx,centerFeatherPx,outputW,outputH;
    };
    static_assert(sizeof(Params)==48);
    void Draw(UINT current,UINT previous,ID3D11ShaderResourceView* rawFlow,
              float horizon,int grid){
        ID3D11ShaderResourceView* views[3]{frameSRV[current].Get(),rawFlow,frameSRV[previous].Get()};
        // Optical flow and extrapolation use the captured client resolution;
        // presentation covers the full monitor, with no selectable letterboxing.
        const Params p{horizon,1.0f/float(srcW),1.0f/float(srcH),0.12f,
              float((srcW+UINT(grid)-1)/UINT(grid)),
              float((srcH+UINT(grid)-1)/UINT(grid)),
              float(grid),0.0f,
              float(g_centerRadiusPx.load(std::memory_order_relaxed)),
              float(g_centerFeatherPx.load(std::memory_order_relaxed)),
              float(screenW),float(screenH)};
        ctx11->UpdateSubresource(constants.Get(),0,nullptr,&p,0,0);
        D3D11_VIEWPORT vp{0,0,float(screenW),float(screenH),0,1};
        ctx11->RSSetViewports(1,&vp);
        ctx11->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
        ctx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx11->VSSetShader(vs.Get(),nullptr,0);ctx11->PSSetShader(ps.Get(),nullptr,0);
        ctx11->PSSetConstantBuffers(0,1,constants.GetAddressOf());
        ctx11->VSSetConstantBuffers(0,1,constants.GetAddressOf());
        ctx11->PSSetShaderResources(0,3,views);
        ctx11->VSSetShaderResources(0,3,views); // source's scene-cut detector runs in VS
        ctx11->PSSetSamplers(0,1,sampler.GetAddressOf());
        ctx11->VSSetSamplers(0,1,sampler.GetAddressOf());
        ctx11->Draw(3,0);
        ID3D11ShaderResourceView* nullViews[3]{};
        ctx11->PSSetShaderResources(0,3,nullViews);
        ctx11->VSSetShaderResources(0,3,nullViews);
        Present(horizon==0.f);
    }
    void PresentReal(UINT current,int grid){
        // Real frames also sample the leased WGC texture directly into the flip buffer.
        Draw(current,current,nullptr,0.f,grid);
    }
    void Present(bool /*real*/){
        // Present with sync=0; request tearing only when supported and enabled
        // on the swapchain.  This leaves actual Independent Flip up to Windows.
        const UINT flags=allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        Check(swap->Present(0,flags),"DX11 low-latency Present");
    }
};

struct Flow11 {
    DxDevices* owner{};
    HMODULE library{};
    NV_OF_D3D11_API_FUNCTION_LIST funcs{};
    NvOFHandle handle{};
    NvOFGPUBufferHandle frames[kNumCapture]{};
    NvOFGPUBufferHandle motion{};
    ComPtr<ID3D11Texture2D> motionTexture;
    ComPtr<ID3D11ShaderResourceView> motionSRV;
    void Init(DxDevices& d,int grid){
        owner=&d;
        library=LoadLibraryW(L"nvofapi64.dll");if(!library)throw Error("nvofapi64.dll missing");
        auto entry=(decltype(&NvOFAPICreateInstanceD3D11))GetProcAddress(library,"NvOFAPICreateInstanceD3D11");
        if(!entry)throw Error("NvOFAPICreateInstanceD3D11 missing");
        NvCheck(entry(NV_OF_API_VERSION,&funcs),"NVOFA D3D11 API initialization");
        if(!funcs.nvCreateOpticalFlowD3D11||!funcs.nvOFInit||!funcs.nvOFExecute||
            !funcs.nvOFRegisterResourceD3D11||!funcs.nvOFUnregisterResourceD3D11)
            throw Error("NVOFA DX11 function list incomplete");
        NvCheck(funcs.nvCreateOpticalFlowD3D11(d.dev11.Get(),d.ctx11.Get(),&handle),"NVOFA Create D3D11");
        NV_OF_INIT_PARAMS init{};init.width=d.srcW;init.height=d.srcH;
        init.outGridSize=grid==1?NV_OF_OUTPUT_VECTOR_GRID_SIZE_1:
            grid==2?NV_OF_OUTPUT_VECTOR_GRID_SIZE_2:NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        init.mode=NV_OF_MODE_OPTICALFLOW;init.perfLevel=NV_OF_PERF_LEVEL_FAST;
        init.enableOutputCost=NV_OF_FALSE;
        init.enableExternalHints=NV_OF_FALSE;init.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;
        NvCheck(funcs.nvOFInit(handle,&init),"NVOFA init (selected grid FAST, ABGR8)");
        // Sharing is only valid if OFA accepts this exact D3D11 texture format.
        // The history texture is registered once and sampled directly.
        UINT count=0;
        NvCheck(funcs.nvOFGetSurfaceFormatCountD3D11(handle,NV_OF_BUFFER_USAGE_INPUT,NV_OF_MODE_OPTICALFLOW,&count),"NVOFA query input format count");
        if(!count||count>32)throw Error("NVOFA input formats unavailable");
        std::vector<DXGI_FORMAT> formats(count);
        NvCheck(funcs.nvOFGetSurfaceFormatD3D11(handle,NV_OF_BUFFER_USAGE_INPUT,NV_OF_MODE_OPTICALFLOW,formats.data()),"NVOFA query input formats");
        if(std::find(formats.begin(),formats.end(),DXGI_FORMAT_B8G8R8A8_UNORM)==formats.end())
            throw Error("NVOFA cannot accept BGRA capture history");
        const UINT fw=(d.srcW+UINT(grid)-1)/UINT(grid),fh=(d.srcH+UINT(grid)-1)/UINT(grid);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=fw;desc.Height=fh;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R16G16_SINT;desc.SampleDesc.Count=1;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        Check(d.dev11->CreateTexture2D(&desc,nullptr,&motionTexture),"Create NvOFA output texture");
        NvCheck(funcs.nvOFRegisterResourceD3D11(handle,motionTexture.Get(),&motion),"NVOFA register motion output");
        Check(d.dev11->CreateShaderResourceView(motionTexture.Get(),nullptr,&motionSRV),"NvOFA motion SRV");
    }
    void Reap(){
        const UINT64 done=owner->Completed();
        for(int i=0;i<kNumCapture;++i){
            if(!owner->retiring[i]||owner->retiring[i]>done)continue;
            if(frames[i]){
                NvCheck(funcs.nvOFUnregisterResourceD3D11(frames[i]),"Unregister retired WGC surface");
                frames[i]=nullptr;
            }
            owner->ReleaseLease(i);
        }
    }
    void Register(int slot){
        if(frames[slot])return;
        if(!owner->leases[slot]||!owner->history[slot])throw Error("Missing WGC lease");
        // Register only a live lease; never cache a WGC texture after check-in.
        NvCheck(funcs.nvOFRegisterResourceD3D11(handle,owner->history[slot].Get(),&frames[slot]),
            "Zero-copy rejected: NvOFA direct WGC registration");
    }
    void Calculate(int current,int previous,bool continuous){
        Register(current);Register(previous);
        NV_OF_EXECUTE_INPUT_PARAMS in{};
        in.inputFrame=frames[current];in.referenceFrame=frames[previous];
        in.disableTemporalHints=continuous?NV_OF_FALSE:NV_OF_TRUE;
        NV_OF_EXECUTE_OUTPUT_PARAMS out{};out.outputBuffer=motion;
        NvCheck(funcs.nvOFExecute(handle,&in,&out),"NvOFA D3D11 Execute");
    }
    ~Flow11(){
        if(owner)owner->DrainForShutdown();
        if(handle){
            for(auto &f:frames)if(f&&funcs.nvOFUnregisterResourceD3D11)funcs.nvOFUnregisterResourceD3D11(f);
            if(motion&&funcs.nvOFUnregisterResourceD3D11)funcs.nvOFUnregisterResourceD3D11(motion);
            if(funcs.nvOFDestroy)funcs.nvOFDestroy(handle);
        }
        if(library)FreeLibrary(library);
    }
};

// A user may Alt+Tab or minimize; neither is an exit. Check the original
// HWND/PID identity and, when permitted, the process termination signal.
struct TrackedGame {
    Game game;
    HANDLE process{};
    explicit TrackedGame(Game g):game(std::move(g)) {
        process=OpenProcess(SYNCHRONIZE,FALSE,game.pid);
    }
    ~TrackedGame(){if(process)CloseHandle(process);}
    bool Exited() const {
        if(process && WaitForSingleObject(process,0)==WAIT_OBJECT_0)return true;
        DWORD owner=0;
        return !IsWindow(game.window)||
            GetWindowThreadProcessId(game.window,&owner)==0||owner!=game.pid;
    }
};
enum class SessionOutcome { Stop, Recreate };

// One session owns ALL WGC capture, D3D11, and NvOFA resources. Returning from
// this function destroys them in reverse lifetime order before another attempt.
SessionOutcome EngineSession(const Game& game,const TrackedGame& tracked){
    try {
        if(tracked.Exited())return SessionOutcome::Stop;
        // D3D11 resources are created on this single render thread.
        OverlayWindow overlay;overlay.Init(game.window);
        WgcApartment apartment;
        const auto item=WgcCapture::ItemForWindow(game.window);
        const auto size=item.Size();
        const UINT gameW=UINT(size.Width),gameH=UINT(size.Height);
        MONITORINFOEXW monitorInfo{};monitorInfo.cbSize=sizeof(monitorInfo);
        UINT monitorHz=320;
        if(GetMonitorInfoW(MonitorFromWindow(game.window,MONITOR_DEFAULTTONEAREST),&monitorInfo)){
            DEVMODEW dm{};dm.dmSize=sizeof(dm);
            if(EnumDisplaySettingsW(monitorInfo.szDevice,ENUM_CURRENT_SETTINGS,&dm)&&dm.dmDisplayFrequency>=30)
                monitorHz=dm.dmDisplayFrequency;
        }
        // Capture the selected window directly (full WGC content extent).
        const UINT sourceW=gameW,sourceH=gameH;
        if(sourceW<16||sourceH<16||sourceW>8192||sourceH>8192)
            throw Error("Invalid game native capture resolution");
        if(session)lstrcpynW(session->stage,L"Strict zero-copy WGC initialization",96);
        DxDevices dx;dx.Init(overlay.hwnd,item,sourceW,sourceH,overlay.width,overlay.height);
        if(session)lstrcpynW(session->stage,L"GPU capture and presentation",96);
        Status(L"Running - WGC direct surfaces");

        constexpr int ofGrid=kOpticalFlowGrid;
        Flow11 flow;flow.Init(dx,ofGrid);
        // One graphics owner; WGC notification thread only signals a handle.
        // Keep source timestamps for motion and arrival/presentation clocks for pacing.
        LARGE_INTEGER freq{};QueryPerformanceFrequency(&freq);
        if(freq.QuadPart<=0)throw Error("QPC frequency unavailable");
        auto now=[]()->LONGLONG{LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;};
        auto ticks=[&](double ms)->LONGLONG{return static_cast<LONGLONG>(ms*double(freq.QuadPart)/1000.0);};
        auto ms=[&](LONGLONG t)->double{return 1000.0*double(t)/double(freq.QuadPart);};
        struct Timer {
            HANDLE handle{CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS)};
            Timer(){if(!handle)handle=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_ALL_ACCESS);}
            ~Timer(){if(handle)CloseHandle(handle);}
        } timer;
        if(!timer.handle)throw Error("Prediction timer creation failed",GetLastError());
        const int originalPriority=GetThreadPriority(GetCurrentThread());
        struct PriorityRestore {int prior;~PriorityRestore(){if(prior!=THREAD_PRIORITY_ERROR_RETURN)SetThreadPriority(GetCurrentThread(),prior);}} priority{originalPriority};
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        bool slotHeld=false;
        auto waitActivity=[&](LONGLONG deadline,bool needSlot){
            const LONGLONG remaining=deadline-now();
            if(remaining<=0)return;
            LARGE_INTEGER due{};
            due.QuadPart=-std::max<LONGLONG>(1,static_cast<LONGLONG>(double(remaining)*10000000.0/double(freq.QuadPart)));
            if(!SetWaitableTimer(timer.handle,&due,0,nullptr,nullptr,FALSE))throw Error("Set prediction timer",GetLastError());
            HANDLE handles[3]{dx.capture.event.get(),timer.handle,dx.frameReady};
            const DWORD count=needSlot&&!slotHeld?3u:2u;
            const DWORD r=MsgWaitForMultipleObjectsEx(count,handles,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if(count==3&&r==WAIT_OBJECT_0+2)slotHeld=true;
            if(r==WAIT_FAILED)throw Error("WGC scheduler wait",GetLastError());
            CancelWaitableTimer(timer.handle);
        };
        auto acquireSlot=[&]()->bool{
            if(slotHeld)return true;
            const DWORD r=WaitForSingleObject(dx.frameReady,0);
            if(r==WAIT_OBJECT_0){slotHeld=true;return true;}
            if(r==WAIT_TIMEOUT)return false;
            throw Error("DXGI frame latency wait failed",GetLastError());
        };
        LONGLONG priorSource=0,arrival=0,realPresented=0,lastPresented=0;
        double interval=16.667,pair=0.0,slotMs=3.125;
        int current=-1,previous=-1;
        uint64_t captureSerial=0,lastFlowSerial=0;
        bool flowReady=false,realPending=false,wasFocused=false;
        unsigned nextPrediction=1,maxPredictions=0;
        int measurements=0,warmup=0;
        while(g_running.load(std::memory_order_acquire)){
            if(session){g_fgMode.store(session->mode);g_targetHz.store(session->target);g_centerRadiusPx.store(session->radius);g_centerFeatherPx.store(session->feather);}
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            if(tracked.Exited()){g_running.store(false,std::memory_order_release);break;}
            DWORD foregroundPid=0;
            HWND foreground=GetForegroundWindow();
            if(foreground)GetWindowThreadProcessId(foreground,&foregroundPid);
            const bool focused=!IsIconic(game.window)&&foregroundPid==game.pid;
            if(focused && overlay.SyncToMonitor(game.window))throw DisplayChanged{};
            flow.Reap();
            if(!focused){
                overlay.SetVisible(false);wasFocused=false;flowReady=false;realPending=false;
                dx.RetireExcept(-1,-1);
                // Release queued WGC frames while unfocused so resume starts fresh.
                if(auto dropped=dx.capture.Latest(0))dropped.Close();
                waitActivity(now()+ticks(8),false);
                continue;
            }
            if(!wasFocused){
                wasFocused=true;warmup=2;measurements=0;current=-1;previous=-1;
                captureSerial=lastFlowSerial=0;priorSource=0;interval=16.667;pair=0;
                flowReady=false;realPending=false;maxPredictions=0;nextPrediction=1;
                realPresented=lastPresented=0;
            }
            LARGE_INTEGER stamp{};
            const int next=dx.FreeSlot();
            if(next<0){waitActivity(now()+ticks(0.5),false);continue;}
            // New real frames always win the mailbox, including when a present slot
            // becomes available while we were waiting on the timer.
            if(dx.Grab(game.window,next,0,stamp)){
                arrival=now();++captureSerial;
                previous=current;current=next;pair=0.0;
                dx.RetireExcept(current,previous);
                if(priorSource>0&&stamp.QuadPart>priorSource){
                    const double measured=ms(stamp.QuadPart-priorSource);
                    if(measured>=1.0&&measured<=80.0){
                        pair=measured;
                        interval=measurements++==0?measured:std::clamp(interval*.60+measured*.40,1.0,80.0);
                    }
                }
                priorSource=stamp.QuadPart;
                realPending=true;flowReady=false;maxPredictions=0;nextPrediction=1;
                if(warmup>0)--warmup;
            }
            if(realPending){
                if(!acquireSlot()){waitActivity(now()+ticks(8),true);continue;}
                dx.PresentReal(UINT(current),ofGrid);slotHeld=false;
                realPresented=lastPresented=now();
                overlay.SetVisible(true);realPending=false;
                const int mode=std::clamp(g_fgMode.load(std::memory_order_relaxed),0,6);
                const int configured=g_targetHz.load(std::memory_order_relaxed);
                const int target=std::clamp(configured==0?int(monitorHz):configured,30,1000);
                // Dynamic target can exceed x6 when the real FPS is very low.
                // Bound the maximum prediction horizon to one measured source interval.
                slotMs=mode==0?1000.0/target:interval/double(std::max(1,mode));
                maxPredictions=mode==0?std::min(12u,unsigned(std::max(0.0,std::ceil(interval/slotMs)-1.0))):
                    (mode>1?unsigned(mode-1):0u);
                if(mode==0&&interval<=1.05*slotMs)maxPredictions=0;
                nextPrediction=1;
                const LONGLONG expiry=arrival+ticks(interval*1.10);
                if(maxPredictions>0&&previous>=0&&pair>0&&warmup==0&&
                    realPresented+ticks(slotMs*.70)<expiry){
                    const bool continuous=lastFlowSerial!=0&&lastFlowSerial+1==captureSerial;
                    flow.Calculate(current,previous,continuous);
                    lastFlowSerial=captureSerial;flowReady=true;
                }
                continue;
            }
            if(!flowReady||nextPrediction>maxPredictions){
                waitActivity(now()+ticks(8),false);continue;
            }
            const LONGLONG t=now();
            // Pacing begins at actual real submission, not WGC's earlier source
            // timestamp. Do not consume fake slots before the real was submitted.
            const LONGLONG expiry=arrival+ticks(interval*1.10);
            if(t>=expiry){flowReady=false;continue;}
            const LONGLONG due=std::max(realPresented+ticks(slotMs*nextPrediction),
                                        lastPresented+ticks(slotMs*.85));
            if(due>=expiry){flowReady=false;continue;}
            if(t<due){waitActivity(due,false);continue;}
            if(!acquireSlot()){waitActivity(expiry,true);continue;}
            // No burst catch-up; use the newest due prediction, at most one per
            // present opportunity. Never extrapolate more than one source pair.
            const double elapsed=ms(t-realPresented);
            const unsigned dueIndex=std::min(maxPredictions,unsigned(std::max(1.0,std::floor(elapsed/slotMs))));
            nextPrediction=std::max(nextPrediction,dueIndex);
            const float fraction=float(std::clamp(elapsed/pair,0.0,1.0));
            dx.Draw(UINT(current),UINT(previous),flow.motionSRV.Get(),fraction,ofGrid);
            slotHeld=false;lastPresented=now();++nextPrediction;
        }
        overlay.SetVisible(false);
        Status(L"Stopped");
    }catch(const DisplayChanged&){
        return SessionOutcome::Recreate;
    }catch(const winrt::hresult_error& e){
        if(!tracked.Exited()&&!g_userStopRequested.load())Status(std::wstring(L"FG error: WGC ")+e.message().c_str());
        g_running.store(false,std::memory_order_release);
    }catch(const std::exception& e){
        if(!tracked.Exited() && !g_userStopRequested.load())
            Status(L"FG error: "+Wide(e.what()));
        else Status(L"Stopped");
        g_running.store(false,std::memory_order_release);
    }
    return SessionOutcome::Stop;
}

void Engine(Game game){
    Status(L"Starting FG...");
    TrackedGame tracked(game);
    // After repeated invalidations in a short time, report failure rather
    // than spin/reinitialize indefinitely. Full teardown is mandatory.
    ULONGLONG windowStart=GetTickCount64();
    unsigned restartCount=0;
    while(!g_userStopRequested.load(std::memory_order_acquire) && !tracked.Exited()){
        g_running.store(true,std::memory_order_release);
        const SessionOutcome outcome=EngineSession(game,tracked);
        if(outcome!=SessionOutcome::Recreate || tracked.Exited() ||
           g_userStopRequested.load(std::memory_order_acquire))break;
        const ULONGLONG now=GetTickCount64();
        if(now-windowStart>10000){windowStart=now;restartCount=0;}
        if(++restartCount>3){
            Status(L"Source error: WGC capture repeatedly invalidated. "
                   L"Full capture restart failed; check the display mode and driver.");
            break;
        }
        // Allow transient display-mode/topology changes to settle before
        // building a NEW D3D11 device, shared textures and NvOFA state.
        for(unsigned i=0;i<10&&!g_userStopRequested.load()&&!tracked.Exited();++i)
            Sleep(10);
    }
    g_running.store(false,std::memory_order_release);
    g_lastGameExited.store(tracked.Exited(),std::memory_order_release);
    // Publish completion only AFTER all sessions and capture threads have
    // returned; never treat a transient WGC capture recreation as a full stop.
    g_engineFinished.store(true,std::memory_order_release);
    if(g_control)PostMessageW(g_control,WM_FG_STOPPED,tracked.Exited()?1u:0u,0);
}
#include "session_process.inc"



void InitUiStyle(HWND hwnd){
    if(!g_bgBrush)g_bgBrush=CreateSolidBrush(kBackground);
    if(!g_panelBrush)g_panelBrush=CreateSolidBrush(kPanel);
    if(!g_fieldBrush)g_fieldBrush=CreateSolidBrush(kField);
    if(!g_font)g_font=CreateFontW(-17,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!g_fontBold)g_fontBold=CreateFontW(-18,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI Semibold");
    if(!g_fontSmall)g_fontSmall=CreateFontW(-13,0,0,0,FW_MEDIUM,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!g_fontHero)g_fontHero=CreateFontW(-32,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    const BOOL enabled=TRUE;
    DwmSetWindowAttribute(hwnd,static_cast<DWMWINDOWATTRIBUTE>(20),&enabled,sizeof(enabled));
    SetWindowTheme(hwnd,L"DarkMode_Explorer",nullptr);
}
void StyleControl(HWND control,bool bold=false,bool useSmallFont=false){
    if(!control)return;
    SendMessageW(control,WM_SETFONT,(WPARAM)(useSmallFont?g_fontSmall:(bold?g_fontBold:g_font)),TRUE);
    SetWindowTheme(control,L"DarkMode_Explorer",nullptr);
}
HWND UiLabel(HWND parent,const wchar_t* text,int x,int y,int w,int h,bool useSmallFont=false){
    HWND c=CreateWindowW(L"STATIC",text,WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,h,parent,nullptr,nullptr,nullptr);
    StyleControl(c,false,useSmallFont);
    return c;
}
void PaintRoundBox(HDC dc,int l,int t,int r,int b,COLORREF bg,COLORREF border,int radius=20){
    HPEN pen=CreatePen(PS_SOLID,1,border);
    HBRUSH brush=CreateSolidBrush(bg);
    HGDIOBJ op=SelectObject(dc,pen),ob=SelectObject(dc,brush);
    RoundRect(dc,l,t,r,b,radius,radius);
    SelectObject(dc,op);SelectObject(dc,ob);
    DeleteObject(pen);DeleteObject(brush);
}
void DrawUi(HWND hwnd,HDC dc){
    RECT cr{};GetClientRect(hwnd,&cr);
    FillRect(dc,&cr,g_bgBrush);
    // Branded header: subtle divider and cyan accent.
    RECT top{24,79,cr.right-24,81};HBRUSH line=CreateSolidBrush(RGB(45,62,75));
    FillRect(dc,&top,line);DeleteObject(line);
    RECT accent{24,79,124,81};HBRUSH cyan=CreateSolidBrush(kAccent);
    FillRect(dc,&accent,cyan);DeleteObject(cyan);
    PaintRoundBox(dc,20,96,cr.right-20,199,kPanel,RGB(40,58,70));
    PaintRoundBox(dc,20,210,cr.right-20,306,kPanel,RGB(40,58,70));
    PaintRoundBox(dc,20,316,cr.right-20,390,kPanel,RGB(40,58,70));
    PaintRoundBox(dc,20,397,cr.right-20,476,kPanel,RGB(40,58,70));
    HICON icon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(kIconId));
    if(icon)DrawIconEx(dc,24,18,icon,52,52,0,nullptr,DI_NORMAL);
    SetBkMode(dc,TRANSPARENT);
    HFONT original=(HFONT)SelectObject(dc,g_fontHero);
    SetTextColor(dc,kText);TextOutW(dc,86,19,L"FG",2);
    SelectObject(dc,g_fontSmall);
    SetTextColor(dc,kMuted);
    const wchar_t* sub=L"FRAME GENERATION  /  LOW LATENCY";
    TextOutW(dc,87,58,sub,(int)wcslen(sub));
    SelectObject(dc,original);
}
void DrawUiButton(const DRAWITEMSTRUCT* ds){
    HDC dc=ds->hDC;RECT r=ds->rcItem;
    bool enabled=(ds->itemState&ODS_DISABLED)==0;
    bool hover=(ds->itemState&ODS_SELECTED)!=0;
    bool isStart=ds->CtlID==kStart;
    COLORREF fill=isStart?(enabled?(hover?RGB(37,174,171):RGB(41,209,203)):RGB(39,88,91)):
                 (hover?RGB(51,68,85):RGB(38,52,66));
    PaintRoundBox(dc,r.left+1,r.top+1,r.right-1,r.bottom-1,fill,isStart?RGB(65,233,230):RGB(70,89,105),12);
    wchar_t caption[80]{};GetWindowTextW(ds->hwndItem,caption,80);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,isStart?RGB(5,18,25):(enabled?kText:kMuted));
    HFONT old=(HFONT)SelectObject(dc,g_fontBold);
    DrawTextW(dc,caption,-1,&r,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
    SelectObject(dc,old);
}
void DrawUiCombo(const DRAWITEMSTRUCT* ds){
    if(ds->itemID==UINT(-1)&& (ds->itemState&ODS_COMBOBOXEDIT)==0)return;
    const bool selected=(ds->itemState&ODS_SELECTED)!=0;
    COLORREF background=selected?RGB(30,92,104):kField;
    HBRUSH brush=CreateSolidBrush(background);
    FillRect(ds->hDC,&ds->rcItem,brush);DeleteObject(brush);
    int id=ds->itemID==UINT(-1)?int(SendMessageW(ds->hwndItem,CB_GETCURSEL,0,0)):int(ds->itemID);
    if(id<0)return;
    wchar_t text[260]{};
    SendMessageW(ds->hwndItem,CB_GETLBTEXT,id,(LPARAM)text);
    RECT rc=ds->rcItem;rc.left+=12;
    SetBkMode(ds->hDC,TRANSPARENT);
    SetTextColor(ds->hDC,kText);
    HFONT old=(HFONT)SelectObject(ds->hDC,g_font);
    DrawTextW(ds->hDC,text,-1,&rc,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    SelectObject(ds->hDC,old);
}

LRESULT CALLBACK OverlayProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_INPUT){
        RAWINPUT input{};UINT bytes=sizeof(input);
        if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&input,&bytes,sizeof(RAWINPUTHEADER))==sizeof(input) &&
           input.header.dwType==RIM_TYPEMOUSE && !(input.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)){
            fgwarp::mouseX.fetch_add(input.data.mouse.lLastX);fgwarp::mouseY.fetch_add(input.data.mouse.lLastY);
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    return DefWindowProcW(hwnd,msg,wp,lp);
}
LRESULT CALLBACK ControlProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        InitUiStyle(hwnd);
        UiLabel(hwnd,L"SOURCE",36,113,90,21,true);
        g_targets=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
            36,136,335,190,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTarget)),nullptr,nullptr);
        StyleControl(g_targets);
        g_refresh=CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            386,135,140,31,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefresh)),nullptr,nullptr);
        StyleControl(g_refresh);
        UiLabel(hwnd,L"FRAME GENERATION",36,176,180,18,true);
        g_multi=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
            36,229,160,210,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMultiplier)),nullptr,nullptr);
        StyleControl(g_multi);
        for(const wchar_t* mode:{L"Dynamic",L"Off",L"x2",L"x3",L"x4",L"x5",L"x6"})
            SendMessageW(g_multi,CB_ADDSTRING,0,(LPARAM)mode);
        SendMessageW(g_multi,CB_SETCURSEL,0,0);
        UiLabel(hwnd,L"Target FPS",217,225,95,20,true);
        g_dynamicTarget=CreateWindowW(L"EDIT",L"320",WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_AUTOHSCROLL,
            218,247,100,27,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDynamicTarget)),nullptr,nullptr);
        StyleControl(g_dynamicTarget);
        SendMessageW(g_dynamicTarget,EM_SETLIMITTEXT,4,0);
        HWND warpCheck=CreateWindowW(L"BUTTON",L"Frame Warp",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
            36,285,240,30,hwnd,reinterpret_cast<HMENU>(120),nullptr,nullptr);
        StyleControl(warpCheck);EnableWindow(warpCheck,FALSE);SetWindowTextW(warpCheck,L"Frame Warp (off - zero copy)");
        g_start=CreateWindowW(L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            36,331,233,42,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStart)),nullptr,nullptr);
        g_stop=CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE|WS_DISABLED|BS_OWNERDRAW,
            282,331,244,42,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStop)),nullptr,nullptr);
        StyleControl(g_start,true);StyleControl(g_stop,true);
        UiLabel(hwnd,L"CENTER RADIUS (px)",36,407,200,19,true);
        g_centerRadiusEdit=CreateWindowW(L"EDIT",L"0",WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_AUTOHSCROLL,
            36,429,200,30,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCenterRadius)),nullptr,nullptr);
        StyleControl(g_centerRadiusEdit);
        SendMessageW(g_centerRadiusEdit,EM_SETLIMITTEXT,3,0);
        UiLabel(hwnd,L"CENTER FEATHER (px)",285,407,230,19,true);
        g_centerFeatherEdit=CreateWindowW(L"EDIT",L"0",WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_AUTOHSCROLL,
            285,429,200,30,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCenterFeather)),nullptr,nullptr);
        StyleControl(g_centerFeatherEdit);
        SendMessageW(g_centerFeatherEdit,EM_SETLIMITTEXT,3,0);
        // The timer is a fallback for a missed/late worker completion message.
        // It never enables Start while WGC capture is still restarting or tearing down.
        SetTimer(hwnd,kLauncherLifecycleTimer,100,nullptr);
        RefreshGames();return 0;
    }
    case WM_DRAWITEM:{
        const auto* d=reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if(d && d->CtlType==ODT_BUTTON){DrawUiButton(d);return TRUE;}
        if(d && d->CtlType==ODT_COMBOBOX){DrawUiCombo(d);return TRUE;}
        break;
    }
    case WM_MEASUREITEM:{
        auto* m=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if(m && m->CtlType==ODT_COMBOBOX){m->itemHeight=25;return TRUE;}
        break;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)wp;SetBkMode(dc,TRANSPARENT);SetTextColor(dc,kMuted);
        SetBkColor(dc,kBackground);
        // Labels are painted over the matching panel color.
        return (LRESULT)g_panelBrush;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:{
        HDC dc=(HDC)wp;SetTextColor(dc,kText);SetBkColor(dc,kField);
        return (LRESULT)g_fieldBrush;
    }
    case WM_ERASEBKGND:return TRUE;
    case WM_PAINT:{
        PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);
        DrawUi(hwnd,dc);EndPaint(hwnd,&ps);return 0;
    }
    case WM_TIMER:
        if(wp==kLauncherLifecycleTimer &&
           g_engineFinished.load(std::memory_order_acquire) &&
           !IsWindowEnabled(g_start)){
            // Engine owns GPU teardown; only the GUI joins after completion.
            if(g_worker.joinable())g_worker.join();
            EnableWindow(GetDlgItem(hwnd,120),TRUE);EnableWindow(g_start,TRUE);
            EnableWindow(g_stop,FALSE);
            if(g_lastGameExited.exchange(false,std::memory_order_acq_rel))RefreshGames();
        }
        return 0;
    case WM_HOTKEY:
        if(wp==kEmergencyHotkey){
            g_userStopRequested.store(true);
            g_running.store(false);
        }
        return 0;
    case WM_COMMAND:{
        switch(LOWORD(wp)){
        case 120:
            fgwarp::requested.store(false);
            break;
        case kRefresh:if(!g_running.load())RefreshGames();break;
        case kMultiplier:
            if(HIWORD(wp)==CBN_SELCHANGE){
                const auto value=SendMessageW(g_multi,CB_GETCURSEL,0,0);
                if(value!=CB_ERR)g_fgMode.store(int(value));
            }
            break;
        case kCenterRadius:
            if(HIWORD(wp)==EN_CHANGE && g_centerRadiusEdit){
                wchar_t value[16]{};GetWindowTextW(g_centerRadiusEdit,value,16);
                g_centerRadiusPx.store(std::min(300ul,wcstoul(value,nullptr,10)),std::memory_order_relaxed);
            }
            break;
        case kCenterFeather:
            if(HIWORD(wp)==EN_CHANGE && g_centerFeatherEdit){
                wchar_t value[16]{};GetWindowTextW(g_centerFeatherEdit,value,16);
                g_centerFeatherPx.store(std::min(300ul,wcstoul(value,nullptr,10)),std::memory_order_relaxed);
            }
            break;
        case kDynamicTarget:
            if(HIWORD(wp)==EN_CHANGE){
                wchar_t value[16]{};GetWindowTextW(g_dynamicTarget,value,16);
                const unsigned hz=wcstoul(value,nullptr,10);
                if(hz>=30&&hz<=1000)g_targetHz.store(int(hz));
            }
            break;
        case kStart:{
            if(g_running.load()||!g_engineFinished.load(std::memory_order_acquire))break;
            auto selection=SendMessageW(g_targets,CB_GETCURSEL,0,0);
            auto validSelection=[&](LRESULT index){
                if(index==CB_ERR||index<0||size_t(index)>=g_games.size())return false;
                const Game& g=g_games[size_t(index)];
                DWORD owner=0;
                return IsWindow(g.window)&&
                    GetWindowThreadProcessId(g.window,&owner)!=0&&owner==g.pid;
            };
            // If CS2 has been closed and relaunched, the old HWND/PID is stale.
            // Refresh the chooser instead of starting a capture on a dead window.
            if(!validSelection(selection)){
                RefreshGames();
                selection=SendMessageW(g_targets,CB_GETCURSEL,0,0);
            }
            if(!validSelection(selection)){
                MessageBoxW(hwnd,L"Open the game and click Refresh before Start.",
                    L"FG",MB_ICONINFORMATION);break;
            }
            const Game game=g_games[size_t(selection)];
            auto chosen=SendMessageW(g_multi,CB_GETCURSEL,0,0);
            g_fgMode.store(chosen==CB_ERR?0:int(chosen));
            if(g_worker.joinable())g_worker.join();
            g_userStopRequested.store(false,std::memory_order_release);
            g_lastGameExited.store(false,std::memory_order_release);
            g_engineFinished.store(false,std::memory_order_release);
            g_running.store(true);EnableWindow(GetDlgItem(hwnd,120),FALSE);EnableWindow(g_start,FALSE);EnableWindow(g_stop,TRUE);
            g_worker=std::thread([game]{RunIsolated(game);});
            // Start gives focus back to the selected game. The presenter never activates.
            SetForegroundWindow(game.window);
            break;
        }
        case kStop:
            g_userStopRequested.store(true);
            g_running.store(false);EnableWindow(g_stop,FALSE);break;
        }
        return 0;
    }
    case WM_FG_STATUS:{
        std::wstring s;{std::lock_guard<std::mutex> l(g_statusLock);s=g_statusText;}
        // No Ready/Running/Stopped display. Keep actionable errors visible.
        if(s.rfind(L"FG error:",0)==0||s.rfind(L"Source error:",0)==0)
            MessageBoxW(hwnd,s.c_str(),L"FG error",MB_OK|MB_ICONERROR);
        return 0;
    }
    case WM_FG_STOPPED:
        // A session can temporarily stop for WGC capture recovery. Only the final
        // outer Engine() completion is allowed to unlock another Start.
        if(!g_engineFinished.load(std::memory_order_acquire))return 0;
        if(g_worker.joinable())g_worker.join();
        EnableWindow(GetDlgItem(hwnd,120),TRUE);EnableWindow(g_start,TRUE);EnableWindow(g_stop,FALSE);
        if(g_lastGameExited.exchange(false,std::memory_order_acq_rel))RefreshGames();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd,kLauncherLifecycleTimer);
        g_userStopRequested.store(true);
        g_running.store(false);
        if(g_worker.joinable())g_worker.join();
        UnregisterHotKey(hwnd,kEmergencyHotkey);
        if(g_font)DeleteObject(g_font);if(g_fontBold)DeleteObject(g_fontBold);
        if(g_fontSmall)DeleteObject(g_fontSmall);if(g_fontHero)DeleteObject(g_fontHero);
        if(g_bgBrush)DeleteObject(g_bgBrush);if(g_panelBrush)DeleteObject(g_panelBrush);
        if(g_fieldBrush)DeleteObject(g_fieldBrush);
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR args,int){
    // Obtain physical monitor/window dimensions rather than DPI-virtualized pixels.
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    WNDCLASSW o{};o.hInstance=instance;o.lpfnWndProc=OverlayProc;
    o.lpszClassName=L"FGDXGIOverlay";o.hCursor=nullptr;
    o.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    if(!RegisterClassW(&o))return 1;
    if(wcsncmp(args,L"--fg-session ",13)==0)return RunChild(wcstoul(args+13,nullptr,10));
    WNDCLASSW c{};c.hInstance=instance;c.lpfnWndProc=ControlProc;
    c.lpszClassName=L"FGDXGIControl";c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    c.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    c.hbrBackground=CreateSolidBrush(kBackground);
    if(!RegisterClassW(&c))return 1;
    g_control=CreateWindowW(c.lpszClassName,L"FG",WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX&~WS_THICKFRAME,
        CW_USEDEFAULT,CW_USEDEFAULT,570,540,nullptr,nullptr,instance,nullptr);
    if(!g_control)return 2;
    RegisterHotKey(g_control,kEmergencyHotkey,MOD_CONTROL|MOD_SHIFT,VK_F12);
    HICON appIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    if(appIcon){SendMessageW(g_control,WM_SETICON,ICON_BIG,(LPARAM)appIcon);
                SendMessageW(g_control,WM_SETICON,ICON_SMALL,(LPARAM)appIcon);}
    ShowWindow(g_control,SW_SHOW);UpdateWindow(g_control);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    return int(message.wParam);
}
