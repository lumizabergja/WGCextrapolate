// FG V15 Clean v16. CUDA capture, optical flow and extrapolation with DXGI presentation.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <optional>
#include <memory>
#include <vector>
#include <array>
#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <cmath>
#include <cwchar>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <nvFBC.h>
#include <cuda.h>
#include <cudaD3D11.h>
#include <nvOpticalFlowCuda.h>
#include "pixel_types.h"
#include "display_clock.h"
#include "warp_presenter.h"
#include <fstream>
#include <initializer_list>
#include <NvInferRuntime.h>
#include <nvapi.h>
#include "reflex_presenter.h"

using Microsoft::WRL::ComPtr;
namespace {
// Two display/history slots, two possibly older flow inputs, and a producer slot.
// Publication still uses a single latest-frame mailbox.
constexpr int kNumCapture = 5;
constexpr int kOpticalFlowGrid = 4;
constexpr UINT kIconId = 101;
constexpr UINT kEmergencyHotkey = 0x5240;
constexpr int kTarget = 101, kRefresh = 102, kMultiplier = 103, kStart = 104, kStop = 105,
    kDynamicTarget = 107, kCenterRadius = 108, kCenterFeather = 109,
    kWarp = 110,kHud = 111,kGun = 112,kReflex = 113;
constexpr UINT WM_FG_STATUS = WM_APP+77, WM_FG_STOPPED = WM_APP+78;
HWND g_control{},g_targets{},g_multi{},g_start{},g_stop{},g_overlay{},g_dynamicTarget{};
HWND g_centerRadiusEdit{},g_centerFeatherEdit{};
HWND g_refresh{},g_warp{},g_hud{},g_gun{},g_reflex{},g_reflexStatus{};
std::atomic<bool> g_reflexEnabled{true};
std::atomic<ReflexState> g_reflexState{ReflexState::Idle};
std::atomic<bool> g_warpEnabled{true},g_hudEnabled{true},g_gunEnabled{true};
HFONT g_font{},g_fontBold{},g_fontSmall{},g_fontHero{};
HBRUSH g_bgBrush{},g_fieldBrush{},g_panelBrush{};
const COLORREF kBackground=RGB(10,14,22),kPanel=RGB(20,27,38),kField=RGB(26,36,49),kText=RGB(233,244,250),kMuted=RGB(145,160,173),kAccent=RGB(61,226,220);
std::atomic<bool> g_running{false};
// Separate user-requested Stop from a transient invalidated capture session.
// The latter ends one GPU session before the supervisor restarts it.
std::atomic<bool> g_userStopRequested{false};
// The presenter may temporarily stop during NvFBC recovery. Only the outer
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

void Status(const std::wstring& s) {
    {std::lock_guard<std::mutex> l(g_statusLock);g_statusText=s;}
    if(g_control) PostMessageW(g_control,WM_FG_STATUS,0,0);
}
std::runtime_error Error(const std::string& what,HRESULT hr=0) {
    char b[128];std::snprintf(b,sizeof(b),"%s (0x%08lX)",what.c_str(),(unsigned long)hr);
    return std::runtime_error(b);
}
void Check(HRESULT hr,const char* what) {if(FAILED(hr)) throw Error(what,hr);}
void NvCheck(NV_OF_STATUS s,const char* what) {if(s!=NV_OF_SUCCESS)throw Error(what,static_cast<HRESULT>(s));}
struct NvFbcNeedsRecreate : std::runtime_error {
    explicit NvFbcNeedsRecreate(NVFBCRESULT r)
        :std::runtime_error(Error("NvFBC session must be recreated",r).what()){}
};
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
            L"FGDX9Overlay",L"FG",WS_POPUP,r.left,r.top,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!hwnd)throw Error("Create presenter window failed",GetLastError());
        // Fail closed: without capture exclusion the presenter can recursively capture itself.
        if(!SetWindowDisplayAffinity(hwnd,0x00000011))throw Error("WDA_EXCLUDEFROMCAPTURE unsupported",GetLastError());
        // Full-opacity layered click-through overlay: input reaches CS2 underneath.
        if(!SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA))
            throw Error("SetLayeredWindowAttributes failed",GetLastError());
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
    HANDLE frameReady{};
    bool allowTearing{};
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps,warpPS,depthPS;
    std::unique_ptr<fgwarp::Presenter> warp;
    FgReflex reflex;
    uint64_t lastGuidanceSequence{};
    double lastGuidanceHorizon{};
    float MotionInterval(uint64_t sequence,double horizon)const{
        if(!lastGuidanceSequence)return 0.f;
        if(sequence==lastGuidanceSequence)return float(std::clamp(horizon-lastGuidanceHorizon,0.,2.));
        if(sequence==lastGuidanceSequence+1)return float(std::clamp(1.+horizon-lastGuidanceHorizon,0.,2.));
        return 0.f;
    }
    uint64_t warpSequence{}; int64_t warpPrevious{},warpCurrent{};
    double warpHorizon{},ticksPerMs{};UINT warpSrcW{},warpSrcH{};
    void ResetWarpHistory(){lastGuidanceSequence=0;lastGuidanceHorizon=0;if(warp)warp->resetHistory();}
    void DrainWarp(){if(!warp)return;ctx11->ClearState();D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> event;Check(dev11->CreateQuery(&q,&event),"NGX shutdown query");ctx11->End(event.Get());ctx11->Flush();HRESULT r;while((r=ctx11->GetData(event.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE)Sleep(1);Check(r,"NGX drain");warp.reset();}
    ComPtr<ID3D11Buffer> constants;
    UINT screenW{},screenH{},swapFlags{};
    ComPtr<IDXGIAdapter> matchingAdapter;
    void Init(HWND output,UINT outputW,UINT outputH){
        screenW=outputW;screenH=outputH;
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);ticksPerMs=double(frequency.QuadPart)/1000.;
        HMONITOR monitor=MonitorFromWindow(output,MONITOR_DEFAULTTONEAREST);
        ComPtr<IDXGIFactory1> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
        for(UINT i=0;;++i){
            ComPtr<IDXGIAdapter1> a;HRESULT hr=factory->EnumAdapters1(i,&a);
            if(hr==DXGI_ERROR_NOT_FOUND)break;
            Check(hr,"EnumAdapters1");
            DXGI_ADAPTER_DESC1 desc{};a->GetDesc1(&desc);
            if(desc.VendorId!=0x10de)continue;
            for(UINT j=0;;++j){
                ComPtr<IDXGIOutput> out;hr=a->EnumOutputs(j,&out);
                if(hr==DXGI_ERROR_NOT_FOUND)break;
                Check(hr,"Enumerate NVIDIA outputs");
                DXGI_OUTPUT_DESC info{};Check(out->GetDesc(&info),"Read display descriptor");
                if(info.Monitor==monitor){matchingAdapter=a;break;}
            }
            if(matchingAdapter)break;
        }
        if(!matchingAdapter)throw Error("Game monitor must be on the NVIDIA GPU");
        D3D_FEATURE_LEVEL obtained{};
        Check(D3D11CreateDevice(matchingAdapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&dev11,&obtained,&ctx11),"D3D11CreateDevice");
        ComPtr<IDXGIFactory2> f2;Check(matchingAdapter->GetParent(IID_PPV_ARGS(&f2)),"GetParent IDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 sc{};sc.Width=screenW;sc.Height=screenH;
        sc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount=2;sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sc.Scaling=DXGI_SCALING_STRETCH;
        sc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        // DXGI cannot have a literal maximum frame latency of 0: its minimum is 1.
        // Keep zero *software* backlog using the existing latest-frame mailbox and
        // do not submit a new present until DXGI frees a frame slot.
        ComPtr<IDXGIFactory5> factory5;
        if(SUCCEEDED(f2.As(&factory5))){
            BOOL supported=FALSE;
            if(SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &supported,sizeof(supported))) && supported)
                allowTearing=true;
        }
        sc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (allowTearing ? UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u);
        swapFlags=sc.Flags;
        Check(f2->CreateSwapChainForHwnd(dev11.Get(),output,&sc,nullptr,nullptr,&swap),"CreateSwapChainForHwnd low latency");
        Check(swap.As(&lowLatencySwap),"Query IDXGISwapChain2");
        Check(lowLatencySwap->SetMaximumFrameLatency(1),"Set swapchain max latency 1 (DXGI minimum)");
        frameReady=lowLatencySwap->GetFrameLatencyWaitableObject();
        if(!frameReady)throw Error("Swapchain frame latency waitable object unavailable");
        ComPtr<ID3D11Texture2D> bb;Check(swap->GetBuffer(0,IID_PPV_ARGS(&bb)),"GetBuffer");
        Check(dev11->CreateRenderTargetView(bb.Get(),nullptr,&rtv),"CreateRenderTargetView");
        // CUDA has already rendered the image at the output dimensions. DXGI
        // only scans the registered pixel buffer into the same flip-model RTV.
        static const char* shader=R"HLSL(
Buffer<float4> pixels : register(t0);
Texture2D<float4> warped : register(t1);
Buffer<float4> aiDepth : register(t2);
Texture2D<float4> unwarped : register(t3);
Texture2D<float> freezeMask : register(t4);
cbuffer Layout : register(b0) { uint stride; };
float4 VS(uint id : SV_VertexID) : SV_Position {
    return float4(id==2?3.0:-1.0, id==1?3.0:-1.0, 0, 1);
}
float4 PS(float4 position : SV_Position) : SV_Target {
    return pixels.Load(uint(position.y)*stride+uint(position.x)).bgra;
}
float4 WarpPS(float4 position:SV_Position):SV_Target {
    int3 p=int3(int2(position.xy),0);float a=freezeMask.Load(p);
    // Branch for hard freeze: never allow NGX output to contaminate protected pixels.
    if(a>=1.0)return unwarped.Load(p);
    float4 c=warped.Load(p);
    return a>0?lerp(c,unwarped.Load(p),a):c;
}
struct Guidance {float depth:SV_Target0;float2 motion:SV_Target1;};
Guidance DepthPS(float4 position:SV_Position){float4 p=aiDepth.Load(uint(position.y)*stride+uint(position.x));Guidance g;g.depth=p.x;g.motion=p.yz;return g;}
)HLSL";
        ComPtr<ID3DBlob> vertex,pixel;
        Check(D3DCompile(shader,strlen(shader),"FG",nullptr,nullptr,"VS","vs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vertex,nullptr),"Compile DXGI vertex shader");
        Check(D3DCompile(shader,strlen(shader),"FG",nullptr,nullptr,"PS","ps_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&pixel,nullptr),"Compile DXGI pixel shader");
        Check(dev11->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs),"Create vertex shader");
        Check(dev11->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&ps),"Create pixel shader");
        ComPtr<ID3DBlob> warpPixel;
        Check(D3DCompile(shader,strlen(shader),"FG",nullptr,nullptr,"WarpPS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&warpPixel,nullptr),"Compile NGX output shader");
        Check(dev11->CreatePixelShader(warpPixel->GetBufferPointer(),warpPixel->GetBufferSize(),nullptr,&warpPS),"Create NGX output shader");
        ComPtr<ID3DBlob> depthPixel;
        Check(D3DCompile(shader,strlen(shader),"FG",nullptr,nullptr,"DepthPS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&depthPixel,nullptr),"Compile AI depth shader");
        Check(dev11->CreatePixelShader(depthPixel->GetBufferPointer(),depthPixel->GetBufferSize(),nullptr,&depthPS),"Create AI depth shader");
        D3D11_BUFFER_DESC layout{};
        layout.ByteWidth=16; layout.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        Check(dev11->CreateBuffer(&layout,nullptr,&constants),"Create DXGI image layout");
        SetStride();
    }
    ~DxDevices() { reflex.Disable();g_reflexState.store(ReflexState::Idle);try{DrainWarp();}catch(...){warp.reset();} if (frameReady) CloseHandle(frameReady); }
    void SetStride() {
        const UINT data[4]={screenW,0,0,0};
        ctx11->UpdateSubresource(constants.Get(),0,nullptr,data,0,0);
    }
    void ResizeOutput(UINT width, UINT height) {
        if (width==screenW && height==screenH) return;
        DrainWarp();
        ctx11->OMSetRenderTargets(0,nullptr,nullptr);
        rtv.Reset();
        ctx11->Flush();
        Check(swap->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,swapFlags),"Resize DXGI presenter");
        ComPtr<ID3D11Texture2D> back;
        Check(swap->GetBuffer(0,IID_PPV_ARGS(&back)),"Get resized swapchain buffer");
        Check(dev11->CreateRenderTargetView(back.Get(),nullptr,&rtv),"Create resized RTV");
        screenW=width; screenH=height; SetStride();
    }
    void DrawTo(ID3D11ShaderResourceView* image,ID3D11RenderTargetView* target,bool texture){
        D3D11_VIEWPORT viewport{0,0,float(screenW),float(screenH),0,1};
        ctx11->RSSetViewports(1,&viewport);
        ctx11->OMSetRenderTargets(1,&target,nullptr);
        ctx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx11->VSSetShader(vs.Get(),nullptr,0);
        ctx11->PSSetShader(texture?warpPS.Get():ps.Get(),nullptr,0);
        ctx11->PSSetConstantBuffers(0,1,constants.GetAddressOf());
        ctx11->PSSetShaderResources(texture?1:0,1,&image);
        if(texture){ID3D11ShaderResourceView* protectedViews[]={warp->colorSRV.Get(),warp->maskSRV.Get()};ctx11->PSSetShaderResources(3,2,protectedViews);}
        ctx11->Draw(3,0);
        if(texture){ID3D11ShaderResourceView* empty[2]{};ctx11->PSSetShaderResources(3,2,empty);}
        ID3D11ShaderResourceView* none=nullptr;
        ctx11->PSSetShaderResources(texture?1:0,1,&none);
        ctx11->OMSetRenderTargets(0,nullptr,nullptr);
    }
    void Present(ID3D11ShaderResourceView* image,ID3D11ShaderResourceView* depthImage,unsigned freezeFlags){
        if(g_warpEnabled.load(std::memory_order_relaxed)&&depthImage){
            if(!warp){
                warp=std::make_unique<fgwarp::Presenter>();
                warp->open(dev11.Get(),ctx11.Get(),screenW,screenH,warpSrcW,warpSrcH);
            }
            DrawTo(image,warp->inputRTV.Get(),false);
            D3D11_VIEWPORT vp{0,0,float(screenW),float(screenH),0,1};ctx11->RSSetViewports(1,&vp);
            ID3D11RenderTargetView* guidance[]={warp->depthRTV.Get(),warp->motionRTV.Get()};
            ctx11->OMSetRenderTargets(2,guidance,nullptr);
            ctx11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx11->VSSetShader(vs.Get(),nullptr,0);ctx11->PSSetShader(depthPS.Get(),nullptr,0);
            ctx11->PSSetConstantBuffers(0,1,constants.GetAddressOf());ctx11->PSSetShaderResources(2,1,&depthImage);
            ctx11->Draw(3,0);ctx11->ClearState();
            warp->evaluate(g_centerRadiusPx.load(),g_centerFeatherPx.load(),(freezeFlags&1u)!=0,(freezeFlags&2u)!=0,
                           warpSequence,warpPrevious,warpCurrent,warpHorizon,ticksPerMs);
            DrawTo(warp->outputSRV.Get(),rtv.Get(),true);
            lastGuidanceSequence=warpSequence;lastGuidanceHorizon=warpHorizon;
        }else{
            // Depth may not be ready for the newest real image. Never wait or
            // destroy/recreate NGX on a temporary miss.
            ResetWarpHistory();
            DrawTo(image,rtv.Get(),false);
        }
        reflex.Marker(RENDERSUBMIT_END);reflex.Marker(PRESENT_START);
        const HRESULT presented=swap->Present(0,allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
        reflex.Marker(PRESENT_END);g_reflexState.store(reflex.State(),std::memory_order_relaxed);Check(presented,"DXGI low-latency Present");
    }

};

#include "cuda_driver.h"
#include "capture_cuda.h"
#include "ai_depth.h"
#include "cuda_pipeline.h"
#include "async_flow.h"

struct CaptureMailbox {
    std::mutex mutex;
    int ready{-1};
    bool inUse[kNumCapture]{};
    bool failed{};
    bool recreate{};
    std::wstring reason;
    LARGE_INTEGER capturedAt{};
    HANDLE readyEvent{};
    HANDLE freeEvent{};
    CaptureMailbox(){
        readyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        freeEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!readyEvent||!freeEvent){
            const auto error=GetLastError();
            if(readyEvent)CloseHandle(readyEvent);
            if(freeEvent)CloseHandle(freeEvent);
            throw Error("Mailbox CreateEvent failed",error);
        }
    }
    ~CaptureMailbox(){if(readyEvent)CloseHandle(readyEvent);if(freeEvent)CloseHandle(freeEvent);}
};

// Renderer and producer share GPU resources. Ensure the producer is stopped
// and joined before those resources are destroyed if any render operation throws.
struct CaptureProducerGuard {
    std::thread& worker;
    CaptureMailbox& mailbox;
    OverlayWindow* overlay{};
    ~CaptureProducerGuard() noexcept {
        if(overlay) overlay->SetVisible(false);
        g_running.store(false, std::memory_order_release);
        if (mailbox.freeEvent) SetEvent(mailbox.freeEvent);
        if (mailbox.readyEvent) SetEvent(mailbox.readyEvent);
        if (worker.joinable()) worker.join();
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

// One session owns ALL NvFBC, CUDA, DX11, and NvOFA resources. Returning from
// this function destroys them in reverse lifetime order before another attempt.
SessionOutcome EngineSession(const Game& game,const TrackedGame& tracked){
    try {
        if(tracked.Exited())return SessionOutcome::Stop;
        // CUDA/DX11 shared resources are created on this single render thread.
        OverlayWindow overlay;overlay.Init(game.window);
        RAWINPUTDEVICE mouse{1,2,RIDEV_INPUTSINK,overlay.hwnd};
        if(!RegisterRawInputDevices(&mouse,1,sizeof(mouse)))throw Error("Register raw mouse input",GetLastError());
        struct RawRegistration {~RawRegistration(){fgwarp::acceptInput=false;RAWINPUTDEVICE r{1,2,RIDEV_REMOVE,nullptr};RegisterRawInputDevices(&r,1,sizeof(r));}} rawRegistration;
        fgwarp::acceptInput=false;fgwarp::resetInput();
        MONITORINFOEXW monitorInfo{};monitorInfo.cbSize=sizeof(monitorInfo);
        UINT monitorHz=320;
        if(GetMonitorInfoW(MonitorFromWindow(game.window,MONITOR_DEFAULTTONEAREST),&monitorInfo)){
            DEVMODEW dm{};dm.dmSize=sizeof(dm);
            if(EnumDisplaySettingsW(monitorInfo.szDevice,ENUM_CURRENT_SETTINGS,&dm)&&dm.dmDisplayFrequency>=30)
                monitorHz=dm.dmDisplayFrequency;
        }
        DxDevices dx; dx.Init(overlay.hwnd,overlay.width,overlay.height);
        CudaDriver cuda;
        CaptureCuda capture(cuda); capture.Init(game.pid,dx.matchingAdapter.Get());
        CudaPipeline gpu(cuda,capture); gpu.Init(dx);
        AsyncFlow flow(gpu,cuda,capture.context);
        Status(L"Running - safe CUDA interop + early luma");

        CaptureMailbox mailbox;
        std::array<bool,kNumCapture> retiredPending{};
        std::thread producer([&]{
            try{
                CudaThreadContext binding(cuda,capture.context);
                while(g_running.load(std::memory_order_acquire)){
                    int available=-1;
                    {
                        std::lock_guard<std::mutex> l(mailbox.mutex);
                        for(int i=0;i<kNumCapture;++i)if(!mailbox.inUse[i]&&mailbox.ready!=i){available=i;mailbox.inUse[i]=true;break;}
                    }
                    if(available<0){
                        // A surface becomes available only when the renderer's
                        // CUDA readers finish (or a stale, unread
                        // mailbox entry is overwritten). Sleep(1) adds a
                        // scheduler-quantized delay to the next capture.
                        // The auto-reset event preserves a wakeup even if the
                        // renderer signals it before this thread starts waiting.
                        const DWORD wake=WaitForSingleObject(mailbox.freeEvent,INFINITE);
                        if(wake!=WAIT_OBJECT_0)throw Error("Capture free-slot event failed",GetLastError());
                        continue;
                    }
                    const bool usable=capture.GrabSlot(available);
                    if(!g_running.load())break;
                    if(!usable){
                        {
                            std::lock_guard<std::mutex> lock(mailbox.mutex);
                            mailbox.inUse[available]=false;
                        }
                        Sleep(16);
                        continue;
                    }
                    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
                    {
                        std::lock_guard<std::mutex> l(mailbox.mutex);
                        // latest-only mailbox: never block the capture producer on an older pending frame.
                        if(mailbox.ready>=0){
                            mailbox.inUse[mailbox.ready]=false;
                            // The old unread capture was never submitted to
                            // the renderer. The producer submits grabs in capture order.
                            SetEvent(mailbox.freeEvent);
                        }
                        mailbox.ready=available;mailbox.capturedAt=now;
                    }
                    // Wake the renderer immediately for the newest real frame.
                    SetEvent(mailbox.readyEvent);
                }
            } catch(const NvFbcNeedsRecreate& e){
                // Stop capture before the owner drains the GPU work and releases the session.
                {
                    std::lock_guard<std::mutex> l(mailbox.mutex);
                    mailbox.recreate=true;
                    mailbox.reason=Wide(e.what());
                    g_running.store(false,std::memory_order_release);
                }
                SetEvent(mailbox.readyEvent);
                SetEvent(mailbox.freeEvent);
            } catch(const std::exception& e){
                {
                    std::lock_guard<std::mutex> l(mailbox.mutex);
                    mailbox.failed=true;mailbox.reason=Wide(e.what());g_running.store(false);
                }
                SetEvent(mailbox.readyEvent);
                SetEvent(mailbox.freeEvent);
            }
        });
        CaptureProducerGuard producerGuard{producer,mailbox,&overlay};
        LARGE_INTEGER freq{},last{},now{};QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&last);
        double sourcePeriod=1.0/60.0, pairPeriod=sourcePeriod;
        DisplayClock displayClock;
        LARGE_INTEGER previousCaptureAt{};
        int64_t previousPoseTime=0,currentPoseTime=0;
        int current=-1,previous=-1;
        bool realFramePending=false;
        bool pairEligible=false;
        bool wasFocused=false;
        int resumeWarmup=0;
        int lastMode=-1;
        bool displaySlotReserved=false;
        // The CUDA output is read directly by the DXGI fullscreen draw.
        while(g_running.load(std::memory_order_acquire)){
            flow.Poll();
            // Release after the display fence and both inputs of any active flow job.
            for(int i=0;i<kNumCapture;++i){
                if(!retiredPending[i])continue;
                if(!flow.UsesSlot(i) && gpu.Retired(i)){
                    retiredPending[i]=false;
                    {
                        std::lock_guard<std::mutex> guard(mailbox.mutex);
                        mailbox.inUse[i]=false;
                    }
                    // The presenter and the optical-flow worker have finished with this slot.
                    SetEvent(mailbox.freeEvent);
                }
            }
            MSG message{};
            // Dispatch messages for the flip-model presenter.
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            if(tracked.Exited()){
                g_running.store(false,std::memory_order_release);
                break; // Normal exit, including while CS2 is unfocused.
            }
            DWORD foregroundPid=0;
            HWND foreground=GetForegroundWindow();
            if(foreground) GetWindowThreadProcessId(foreground,&foregroundPid);
            const bool focused=!IsIconic(game.window)&&foregroundPid==game.pid;
            if(focused){
                // CS2 may change the logical scanout resolution on focus gain.
                if(overlay.SyncToMonitor(game.window)){
                    gpu.ReleaseOutputs();
                    dx.ResizeOutput(overlay.width,overlay.height);
                    gpu.CreateOutputs(dx);
                }
            }
            overlay.SetVisible(focused);
            if(!focused) {
                if(wasFocused){fgwarp::acceptInput=false;fgwarp::resetInput();dx.ResetWarpHistory();dx.reflex.Disable();g_reflexState.store(ReflexState::Idle);}
                wasFocused=false;
                pairEligible=false;
                realFramePending=false;
                Sleep(8);
                continue;
            }
            if(!wasFocused) {
                // Alt+Tab resume: don't extrapolate from stale pre-switch captures.
                wasFocused=true;
                fgwarp::resetInput();fgwarp::acceptInput=true;dx.ResetWarpHistory();
                previousPoseTime=currentPoseTime=0;
                resumeWarmup=2;
                displayClock.reset();
                QueryPerformanceCounter(&last);
            }
            int next=-1;
            LARGE_INTEGER captureTime{};
            {
                std::lock_guard<std::mutex> l(mailbox.mutex);
                if(mailbox.ready>=0){next=mailbox.ready;captureTime=mailbox.capturedAt;mailbox.ready=-1;}
            }
            if(next>=0){
                const auto& incoming=capture.frames[next];
                if(current>=0 && (capture.frames[current].width!=incoming.width ||
                                  capture.frames[current].height!=incoming.height))
                    return SessionOutcome::Recreate;
                gpu.NewFrame(next,g_fgMode.load(std::memory_order_relaxed)!=1);
                const int retiring=previous;
                previous=current;current=next;
                // Use producer capture-completion timestamps, not the time the
                // consumer got scheduled. Consumer scheduling jitter otherwise
                // disturbs the source FPS estimate and Dynamic FG cadence.
                if(previousCaptureAt.QuadPart && previous>=0){
                    double observed=double(captureTime.QuadPart-previousCaptureAt.QuadPart)/double(freq.QuadPart);
                    if(observed>0.0005 && observed<0.15){
                        pairPeriod=observed;
                        const auto gap=std::max<uint64_t>(1,incoming.sequence-capture.frames[previous].sequence);
                        const double sample=observed/double(gap);
                        if(sample>1.0/1000.0&&sample<1.0/15.0)
                            sourcePeriod=0.90*sourcePeriod+0.10*sample;
                    }else pairPeriod=sourcePeriod;
                }
                previousPoseTime=currentPoseTime;currentPoseTime=captureTime.QuadPart;
                previousCaptureAt=captureTime;
                last=captureTime;
                realFramePending=true;
                pairEligible=(previous>=0 && resumeWarmup==0);
                if(resumeWarmup>0)--resumeWarmup;
                // Queue a fence after uses of the old texture; release it on a
                // later iteration only after the GPU signals completion.
                if(retiring>=0){
                    if(retiredPending[retiring])throw Error("Capture slot retired twice before GPU release");
                    gpu.Retire(retiring);
                    retiredPending[retiring]=true;
                }
            }
            if(current>=0){
                QueryPerformanceCounter(&now);
                const double elapsed = double(now.QuadPart-last.QuadPart)/double(freq.QuadPart);
                const double clockNow=double(now.QuadPart)/double(freq.QuadPart);
                const int mode=std::clamp(g_fgMode.load(std::memory_order_relaxed),0,6);
                const int configuredTarget=g_targetHz.load(std::memory_order_relaxed);
                const int target=std::clamp(configuredTarget==0?int(monitorHz):configuredTarget,30,1000);
                if(mode!=lastMode){displayClock.reset();lastMode=mode;}
                const bool extrapolate=mode==0 ? sourcePeriod*target>1.05 : mode>1;
                const bool useFlow=pairEligible && extrapolate;
                const auto sequence=capture.frames[current].sequence;
                if(g_warpEnabled.load(std::memory_order_relaxed))gpu.PrepareDepth(current);
                else gpu.PollDepth();
                if(useFlow){
                    gpu.PreparePair(current,previous);
                    flow.Request(current,previous,sequence);
                }
                const bool flowReady=useFlow && flow.Ready(current,sequence);
                const bool cameraReady=g_warpEnabled.load(std::memory_order_relaxed)&&extrapolate&&gpu.DepthReady(current,previous,flowReady);
                const double step=displayPeriod(mode,target,sourcePeriod);
                displayClock.configure(clockNow,step);
                if(next<0&&!realFramePending&&elapsed>sourcePeriod*1.65){
                    // Allow short capture jitter without abruptly freezing FG;
                    // stop extrapolating only when a real-frame gap is large.
                    SwitchToThread();continue;
                }
                // A real frame whose DXGI slot was busy remains urgent even if
                // the mailbox is empty on the following iteration.
                // Off presents each real frame exactly once. In FG mode a real
                // frame takes the next display slot (up to 20% early), not an
                // additional slot that would crowd out extrapolated frames.
                const bool doPresent=extrapolate
                    ? (realFramePending || flowReady || cameraReady) && displayClock.due(clockNow,realFramePending)
                    : realFramePending;
                if(doPresent){
                    // Queue target 0: if a previous Present still occupies the
                    // only allowed DXGI frame slot, keep the newest real source
                    // and recompute extrapolation on the next available slot.
                    // A late flow result must not occupy the sole DXGI frame slot.
                    DWORD ready=displaySlotReserved?WAIT_OBJECT_0:WaitForSingleObject(dx.frameReady,0);
                    if(ready==WAIT_TIMEOUT){
                        // Wait for either a display queue slot OR a newer real
                        // capture. Unlike repeated SwitchToThread polling,
                        // this wakes when an actual event arrives; newly
                        // captured frames still take priority next iteration.
                        HANDLE wakeHandles[2]{dx.frameReady,mailbox.readyEvent};
                        const DWORD wake=MsgWaitForMultipleObjectsEx(2,wakeHandles,2,
                            QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                        if(wake==WAIT_OBJECT_0)ready=WAIT_OBJECT_0;
                        else if(wake==WAIT_OBJECT_0+1 || wake==WAIT_OBJECT_0+2 || wake==WAIT_TIMEOUT)
                            continue;
                        else throw Error("DXGI/capture event wait failed",GetLastError());
                    }
                    if(ready!=WAIT_OBJECT_0)throw Error("DXGI frame latency wait failed",GetLastError());
                    displaySlotReserved=true;
                    // If a newer source arrived while acquiring the waitable
                    // slot, retain the consumed token and select that source.
                    {
                        std::lock_guard<std::mutex> lock(mailbox.mutex);
                        if(mailbox.ready>=0)continue;
                    }
                    // The waitable object may have delayed this submission.
                    // Re-sample QPC after acquiring the slot, not before waiting.
                    QueryPerformanceCounter(&now);
                    const double submitTime=double(now.QuadPart)/double(freq.QuadPart);
                    const double age=double(now.QuadPart-last.QuadPart)/double(freq.QuadPart);
                    const float prediction=(realFramePending||!flowReady)?0.f:
                        float(std::clamp(age/pairPeriod,0.0,1.25));
                    dx.warpSequence=sequence;dx.warpPrevious=previousPoseTime;dx.warpCurrent=currentPoseTime;
                    dx.warpHorizon=prediction;dx.warpSrcW=capture.frames[current].width;dx.warpSrcH=capture.frames[current].height;
                    gpu.Draw(dx,current,previous>=0?previous:current,prediction,
                             float(g_centerRadiusPx.load(std::memory_order_relaxed)),
                             float(g_centerFeatherPx.load(std::memory_order_relaxed)),flowReady);
                    displaySlotReserved=false;
                    realFramePending=false;
                    displayClock.submitted(submitTime);
                }else SwitchToThread();
            } else {
                // No current frame: block on capture publication instead of
                // waiting for an arbitrary 1ms polling tick. Bound the wait
                // so Stop/Alt+Tab and game-exit checks remain responsive.
                const DWORD wake=MsgWaitForMultipleObjectsEx(1,&mailbox.readyEvent,4,
                    QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if(wake!=WAIT_OBJECT_0 && wake!=WAIT_OBJECT_0+1 &&
                    wake!=WAIT_TIMEOUT)throw Error("Capture-ready event wait failed",GetLastError());
            }
        }
        g_running.store(false,std::memory_order_release);
        overlay.SetVisible(false); // Reveal the real game during shutdown.
        // A capture thread may be waiting for a free GPU buffer. Stop waits
        // for the in-flight synchronous NvFBC call to return before freeing GPU memory.
        SetEvent(mailbox.freeEvent);
        SetEvent(mailbox.readyEvent);
        if(producer.joinable())producer.join();
        if(mailbox.recreate && !tracked.Exited() && !g_userStopRequested.load())
            return SessionOutcome::Recreate;
        if(mailbox.failed && !tracked.Exited() && !g_userStopRequested.load())
            Status(L"Source error: "+mailbox.reason);
        else Status(L"Stopped");
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
            Status(L"Source error: NvFBC repeatedly invalidated. "
                   L"Full capture restart failed; check the display mode and driver.");
            break;
        }
        // Allow transient display-mode/topology changes to settle before
        // building a new CUDA session, shared textures and NvOFA state.
        for(unsigned i=0;i<10&&!g_userStopRequested.load()&&!tracked.Exited();++i)
            Sleep(10);
    }
    g_running.store(false,std::memory_order_release);
    g_lastGameExited.store(tracked.Exited(),std::memory_order_release);
    // Publish completion only AFTER all sessions and capture threads have
    // returned; never treat a transient NvFBC recreation as a full stop.
    g_engineFinished.store(true,std::memory_order_release);
    if(g_control)PostMessageW(g_control,WM_FG_STOPPED,tracked.Exited()?1u:0u,0);
}


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
    PaintRoundBox(dc,20,483,cr.right-20,645,kPanel,RGB(40,58,70));
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
    if(msg==WM_INPUT){fgwarp::rawInput(lp);return DefWindowProcW(hwnd,msg,wp,lp);}
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
        auto checkbox=[&](const wchar_t* label,int x,int y,int width,int id){
            HWND c=CreateWindowW(L"BUTTON",label,WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,x,y,width,24,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
            StyleControl(c);SendMessageW(c,BM_SETCHECK,BST_CHECKED,0);return c;
        };
        g_warp=checkbox(L"AI depth + NVIDIA Frame Warp (0-5 ms)",36,493,480,kWarp);
        g_hud=checkbox(L"Freeze HUD regions",36,524,250,kHud);
        g_gun=checkbox(L"Freeze gun region",285,524,250,kGun);
        g_reflex=checkbox(L"NVIDIA Reflex + Boost",36,552,490,kReflex);
        g_reflexStatus=UiLabel(hwnd,L"Reflex: ready (start FG to verify)",36,581,490,20,true);
        UiLabel(hwnd,L"Raw Input - set mouse to 1000 Hz in its software",36,610,490,20,true);
        // The timer is a fallback for a missed/late worker completion message.
        // It never enables Start while NvFBC is still restarting or tearing down.
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
        if(wp==kLauncherLifecycleTimer&&g_reflexStatus){
            static int last=-1;
            const auto state=g_reflexState.load(std::memory_order_relaxed);
            if(last!=int(state)){
                last=int(state);const wchar_t* label=L"Reflex: ready (start FG to verify)";
                switch(state){
                case ReflexState::Off:label=L"Reflex: off";break;
                case ReflexState::Active:label=L"Reflex + Boost: NVAPI accepted / sleep OK";break;
                case ReflexState::ModeRejected:label=L"Reflex: driver rejected sleep mode";break;
                case ReflexState::InitFailed:label=L"Reflex: NVAPI initialization failed";break;
                case ReflexState::SleepRejected:label=L"Reflex: mode accepted, sleep rejected";break;
                case ReflexState::MarkersRejected:label=L"Reflex: mode/sleep accepted, markers rejected";break;
                case ReflexState::ActiveNoSleep:label=L"Reflex: mode accepted, sleep disabled (test)";break;
                default:break;
                }
                SetWindowTextW(g_reflexStatus,label);
            }
        }
        if(wp==kLauncherLifecycleTimer &&
           g_engineFinished.load(std::memory_order_acquire) &&
           !IsWindowEnabled(g_start)){
            // Engine owns GPU teardown; only the GUI joins after completion.
            if(g_worker.joinable())g_worker.join();
            EnableWindow(g_start,TRUE);
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
        case kReflex:if(HIWORD(wp)==BN_CLICKED){g_reflexEnabled.store(SendMessageW(g_reflex,BM_GETCHECK,0,0)==BST_CHECKED);if(!g_running.load())g_reflexState.store(g_reflexEnabled.load()?ReflexState::Idle:ReflexState::Off);}break;
        case kWarp:if(HIWORD(wp)==BN_CLICKED)g_warpEnabled.store(SendMessageW(g_warp,BM_GETCHECK,0,0)==BST_CHECKED);break;
        case kHud:if(HIWORD(wp)==BN_CLICKED)g_hudEnabled.store(SendMessageW(g_hud,BM_GETCHECK,0,0)==BST_CHECKED);break;
        case kGun:if(HIWORD(wp)==BN_CLICKED)g_gunEnabled.store(SendMessageW(g_gun,BM_GETCHECK,0,0)==BST_CHECKED);break;
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
            g_running.store(true);EnableWindow(g_start,FALSE);EnableWindow(g_stop,TRUE);
            g_worker=std::thread([game]{Engine(game);});
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
        // A session can temporarily stop for NvFBC recovery. Only the final
        // outer Engine() completion is allowed to unlock another Start.
        if(!g_engineFinished.load(std::memory_order_acquire))return 0;
        if(g_worker.joinable())g_worker.join();
        EnableWindow(g_start,TRUE);EnableWindow(g_stop,FALSE);
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
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
    // Obtain physical monitor/window dimensions rather than DPI-virtualized pixels.
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    WNDCLASSW o{};o.hInstance=instance;o.lpfnWndProc=OverlayProc;
    o.lpszClassName=L"FGDX9Overlay";o.hCursor=nullptr;
    o.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    if(!RegisterClassW(&o))return 1;
    WNDCLASSW c{};c.hInstance=instance;c.lpfnWndProc=ControlProc;
    c.lpszClassName=L"FGDX9Control";c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    c.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    c.hbrBackground=CreateSolidBrush(kBackground);
    if(!RegisterClassW(&c))return 1;
    g_control=CreateWindowW(c.lpszClassName,L"FG",WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX&~WS_THICKFRAME,
        CW_USEDEFAULT,CW_USEDEFAULT,570,710,nullptr,nullptr,instance,nullptr);
    if(!g_control)return 2;
    RegisterHotKey(g_control,kEmergencyHotkey,MOD_CONTROL|MOD_SHIFT,VK_F12);
    HICON appIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    if(appIcon){SendMessageW(g_control,WM_SETICON,ICON_BIG,(LPARAM)appIcon);
                SendMessageW(g_control,WM_SETICON,ICON_SMALL,(LPARAM)appIcon);}
    ShowWindow(g_control,SW_SHOW);UpdateWindow(g_control);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    return int(message.wParam);
}
