// FG WGC D3D11 direct-texture experimental backend.
// Requires Windows 10 22H2 SDK, C++/WinRT and NVIDIA Optical Flow SDK headers.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <memory>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <thread>
#include <atomic>
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

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace winrt_d3d = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace {
constexpr int kNumCapture = 5; // WGC pool includes current, previous, newest and in-flight buffers.
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
// The presenter may temporarily stop during WGC recovery. Only the outer
// Engine() completion marks the launcher as ready for a fresh Start.
std::atomic<bool> g_engineFinished{true};
std::atomic<bool> g_lastGameExited{false};
constexpr UINT_PTR kLauncherLifecycleTimer = 0xF615;
// 0=Dynamic (target refresh), 1=Off, 2..6=static x2..x6.
std::atomic<int> g_fgMode{1};
std::atomic<int> g_targetHz{0};
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
// Frame-pool resize or window topology changes need a complete WGC/OFA rebuild.
struct WgcNeedsRecreate : std::runtime_error {
    explicit WgcNeedsRecreate(const char* reason) : std::runtime_error(reason) {}
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
            L"FGWGCOverlay",L"FG",WS_POPUP,r.left,r.top,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!hwnd)throw Error("Create presenter window failed",GetLastError());
        // WGC targets the selected GAME HWND, not the display. The presenter's
        // different HWND cannot be captured recursively by this session.
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
    HANDLE frameReady{}; // Borrowed swapchain waitable handle (do not CloseHandle).
    bool allowTearing{};
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> constants;
    UINT srcW{},srcH{},screenW{},screenH{};
    // Native input frames and OFA remain at CS2 resolution. Only the final
    // swapchain matches the current logical output monitor size.
    // NVIDIA fullscreen scaling does not automatically apply to a
    // separate fullscreen flip-model presenter HWND.
    void Init(HWND output,UINT w,UINT h,UINT outputW,UINT outputH){
        srcW=w;srcH=h;screenW=outputW;screenH=outputH;
        // A single D3D11 device is shared by WGC, the predictor, NvOFA and DXGI.
        // The driver-backed default GPU must be the same physical GPU as the capture source.
        D3D_FEATURE_LEVEL obtained{};
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,
            &dev11,&obtained,&ctx11),"D3D11CreateDevice WGC/presenter");
        ComPtr<IDXGIDevice> dxgiDev;
        Check(dev11.As(&dxgiDev),"Query IDXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        Check(dxgiDev->GetAdapter(&adapter),"Query DXGI adapter");
        ComPtr<IDXGIFactory2> f2;Check(adapter->GetParent(IID_PPV_ARGS(&f2)),"GetParent IDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 sc{};sc.Width=screenW;sc.Height=screenH;
        sc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount=2;sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sc.Scaling=DXGI_SCALING_STRETCH;
        sc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        // DXGI cannot have a literal maximum frame latency of 0: its minimum is 1.
        // Keep zero *software* backlog by draining WGC down to its newest frame;
        // do not submit a new Present until DXGI frees a frame slot.
        ComPtr<IDXGIFactory5> factory5;
        if(SUCCEEDED(f2.As(&factory5))){
            BOOL supported=FALSE;
            if(SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &supported,sizeof(supported))) && supported)
                allowTearing=true;
        }
        sc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
            (allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        Check(f2->CreateSwapChainForHwnd(dev11.Get(),output,&sc,nullptr,nullptr,&swap),"CreateSwapChainForHwnd low latency");
        Check(swap.As(&lowLatencySwap),"Query IDXGISwapChain2");
        Check(lowLatencySwap->SetMaximumFrameLatency(1),"Set swapchain max latency 1 (DXGI minimum)");
        frameReady=lowLatencySwap->GetFrameLatencyWaitableObject();
        if(!frameReady)throw Error("Swapchain frame latency waitable object unavailable");
        ComPtr<ID3D11Texture2D> bb;Check(swap->GetBuffer(0,IID_PPV_ARGS(&bb)),"GetBuffer");
        Check(dev11->CreateRenderTargetView(bb.Get(),nullptr,&rtv),"CreateRenderTargetView");
        // No intermediate color allocations: each WGC frame supplies its own
        // D3D11 resource, and Draw samples it through a direct SRV.
        // Fullscreen triangle: PS renders input or extrapolated source directly into the swapchain RTV.
        // Preserve the exact original FG-Fast-Crosshair extrapolation shader.
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
    // The display can switch 1920x1080 <-> 1280x960 on Alt+Tab.
    // Resize only the final output; preserve native capture, NvOFA and shader.
    void ResizeOutput(UINT outputW,UINT outputH){
        if(outputW==screenW&&outputH==screenH)return;
        ctx11->OMSetRenderTargets(0,nullptr,nullptr);
        rtv.Reset();
        ctx11->Flush();
        Check(swap->ResizeBuffers(0,outputW,outputH,DXGI_FORMAT_UNKNOWN,0),"DX11 ResizeBuffers after display mode switch");
        ComPtr<ID3D11Texture2D> bb;
        Check(swap->GetBuffer(0,IID_PPV_ARGS(&bb)),"DX11 GetBuffer after resize");
        Check(dev11->CreateRenderTargetView(bb.Get(),nullptr,&rtv),"DX11 RTV after resize");
        screenW=outputW;screenH=outputH;
    }
    // 48-byte constant buffer: motion settings plus center-only protection.
    struct Params{
        float horizon, invW, invH, maxMotion;
        float flowW, flowH, grid, padding1;
        float centerRadiusPx,centerFeatherPx,outputW,outputH;
    };
    static_assert(sizeof(Params)==48);
    void Draw(ID3D11ShaderResourceView* current,ID3D11ShaderResourceView* previous,
              ID3D11ShaderResourceView* rawFlow,float horizon,int grid){
        ID3D11ShaderResourceView* views[3]{current,rawFlow,previous ? previous : current};
        // Capture, NvOFA and extrapolation remain at the game's native resolution;
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
        // Present with sync=0; request tearing only when supported and enabled
        // on the swapchain.  This leaves actual Independent Flip up to Windows.
        const UINT flags=allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        Check(swap->Present(0,flags),"DX11 low-latency Present");
    }
};

// Selected-window WGC capture: no desktop cropping, D3D9Ex or pixel copies.
struct CaptureWgc {
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token arrivedToken{};
    struct SafeEvent {
        HANDLE handle{};
        ~SafeEvent(){if(handle)CloseHandle(handle);}
    };
    std::shared_ptr<SafeEvent> safeEvent;
    HANDLE arrivalEvent{}; // borrowed; SafeEvent owns it
    UINT width{},height{};
    bool subscribed{};

    static wgc::GraphicsCaptureItem SelectWindow(HWND hwnd){
        if(!wgc::GraphicsCaptureSession::IsSupported())
            throw Error("Windows Graphics Capture is unsupported on this OS/device");
        auto interop=winrt::get_activation_factory<wgc::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(
            hwnd,winrt::guid_of<wgc::GraphicsCaptureItem>(),winrt::put_abi(item)));
        return item;
    }
    static std::pair<UINT,UINT> Size(const wgc::GraphicsCaptureItem& item){
        const auto size=item.Size();
        if(size.Width<16||size.Height<16||size.Width>8192||size.Height>8192)
            throw Error("Unsupported WGC capture size");
        return {UINT(size.Width),UINT(size.Height)};
    }
    void Init(wgc::GraphicsCaptureItem selected,ID3D11Device* device,UINT w,UINT h){
        item=std::move(selected);width=w;height=h;
        ComPtr<IDXGIDevice> dxgi;
        Check(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"WGC DXGI device query");
        winrt::com_ptr<::IInspectable> inspectable;
        Check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspectable.put()),
              "CreateDirect3D11DeviceFromDXGIDevice");
        const auto winrtDevice=inspectable.as<winrt_d3d::IDirect3DDevice>();
        pool=wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice,winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            kNumCapture,{int32_t(width),int32_t(height)});
        session=pool.CreateCaptureSession(item);
        // Windows 10 22H2 supports per-session cursor disabling.
        session.IsCursorCaptureEnabled(false);
        safeEvent=std::make_shared<SafeEvent>();
        safeEvent->handle=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!safeEvent->handle)throw Error("WGC arrival event allocation failed",GetLastError());
        arrivalEvent=safeEvent->handle;
        // In-flight callback owns the event until SetEvent completes, even
        // if Stop destroys the CaptureWgc object concurrently.
        arrivedToken=pool.FrameArrived([signal=safeEvent](auto const&,auto const&) noexcept {
            SetEvent(signal->handle);
        });
        subscribed=true;
        session.StartCapture();
    }
    // Return only the most recent frame; earlier unread frames are closed.
    // No texture data is ever read by the CPU or copied into a staging resource.
    wgc::Direct3D11CaptureFrame DrainLatest(){
        wgc::Direct3D11CaptureFrame newest{nullptr};
        for(;;){
            auto next=pool.TryGetNextFrame();
            if(!next)break;
            if(newest)newest.Close();
            newest=std::move(next);
        }
        return newest;
    }
    void Close() noexcept {
        try{
            if(pool && subscribed){pool.FrameArrived(arrivedToken);subscribed=false;}
            if(session){session.Close();session=nullptr;}
            if(pool){pool.Close();pool=nullptr;}
            item=nullptr;
        }catch(...){ }
        arrivalEvent=nullptr;
        safeEvent.reset();
    }
    ~CaptureWgc(){Close();}
};

struct Flow11 {
    HMODULE library{};
    NV_OF_D3D11_API_FUNCTION_LIST funcs{};
    NvOFHandle handle{};
    NvOFGPUBufferHandle motion{};
    ComPtr<ID3D11Texture2D> motionTexture;
    ComPtr<ID3D11ShaderResourceView> motionSRV;
    void Init(DxDevices& d,int grid){
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
        // The WGC input surface must be directly accepted by NvOFA.
        // Fail closed on format/registration rejection; never copy or convert.
        UINT count=0;
        NvCheck(funcs.nvOFGetSurfaceFormatCountD3D11(handle,NV_OF_BUFFER_USAGE_INPUT,NV_OF_MODE_OPTICALFLOW,&count),"NVOFA query input format count");
        if(!count||count>32)throw Error("NVOFA input formats unavailable");
        std::vector<DXGI_FORMAT> formats(count);
        NvCheck(funcs.nvOFGetSurfaceFormatD3D11(handle,NV_OF_BUFFER_USAGE_INPUT,NV_OF_MODE_OPTICALFLOW,formats.data()),"NVOFA query input formats");
        if(std::find(formats.begin(),formats.end(),DXGI_FORMAT_B8G8R8A8_UNORM)==formats.end())
            throw Error("NVOFA cannot use WGC BGRA surface; no-copy path refused");
        const UINT fw=(d.srcW+UINT(grid)-1)/UINT(grid),fh=(d.srcH+UINT(grid)-1)/UINT(grid);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=fw;desc.Height=fh;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R16G16_SINT;desc.SampleDesc.Count=1;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        Check(d.dev11->CreateTexture2D(&desc,nullptr,&motionTexture),"Create NvOFA output texture");
        NvCheck(funcs.nvOFRegisterResourceD3D11(handle,motionTexture.Get(),&motion),"NVOFA register motion output");
        Check(d.dev11->CreateShaderResourceView(motionTexture.Get(),nullptr,&motionSRV),"NvOFA motion SRV");
    }
    NvOFGPUBufferHandle Register(ID3D11Texture2D* input){
        NvOFGPUBufferHandle registered{};
        NvCheck(funcs.nvOFRegisterResourceD3D11(handle,input,&registered),
                "NVOFA direct WGC resource registration rejected (no fallback)");
        return registered;
    }
    void Unregister(NvOFGPUBufferHandle& resource) noexcept {
        if(resource && funcs.nvOFUnregisterResourceD3D11){
            funcs.nvOFUnregisterResourceD3D11(resource);
            resource=nullptr;
        }
    }
    void Calculate(NvOFGPUBufferHandle current,NvOFGPUBufferHandle previous){
        NV_OF_EXECUTE_INPUT_PARAMS in{};
        in.inputFrame=current;in.referenceFrame=previous;
        NV_OF_EXECUTE_OUTPUT_PARAMS out{};out.outputBuffer=motion;
        NvCheck(funcs.nvOFExecute(handle,&in,&out),"NvOFA D3D11 Execute");
    }
    ~Flow11(){
        if(handle){
            if(motion&&funcs.nvOFUnregisterResourceD3D11)funcs.nvOFUnregisterResourceD3D11(motion);
            if(funcs.nvOFDestroy)funcs.nvOFDestroy(handle);
        }
        if(library)FreeLibrary(library);
    }
};

struct CapturedFrame {
    Flow11* flowOwner{}; // Flow11 outlives all frame slots in EngineSession.
    wgc::Direct3D11CaptureFrame frame{nullptr};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Query> retirement;
    NvOFGPUBufferHandle ofInput{};
    LARGE_INTEGER capturedAt{};
    void Close() noexcept {
        if(flowOwner)flowOwner->Unregister(ofInput);
        srv.Reset();texture.Reset();
        if(frame){
            try{frame.Close();}catch(...){/* Windows device may already be lost */}
            frame=nullptr;
        }
        retirement.Reset();
    }
    ~CapturedFrame(){Close();}
};
using FramePtr = std::unique_ptr<CapturedFrame>;

// GPU completion and WGC frame Close are required EVEN if Present or NvOFA
// throws. Declare this guard after the frame pointers so it runs first on
// both normal completion and exception unwinding.
struct FrameLifetimeGuard {
    DxDevices& dx;
    FramePtr& current;
    FramePtr& previous;
    std::vector<FramePtr>& retired;
    ~FrameLifetimeGuard() noexcept {
        dx.ctx11->ClearState();
        ComPtr<ID3D11Query> done;
        D3D11_QUERY_DESC query{};query.Query=D3D11_QUERY_EVENT;
        if(SUCCEEDED(dx.dev11->CreateQuery(&query,&done))){
            dx.ctx11->End(done.Get());
            dx.ctx11->Flush();
            for(unsigned attempt=0;attempt<1500;++attempt){
                const HRESULT r=dx.ctx11->GetData(done.Get(),nullptr,0,0);
                if(r==S_OK||FAILED(r))break;
                Sleep(1);
            }
        }else dx.ctx11->Flush();
        for(auto& slot:retired){if(slot){slot->Close();slot.reset();}}
        retired.clear();
        if(previous){previous->Close();previous.reset();}
        if(current){current->Close();current.reset();}
    }
};

// Accept only the *original* texture WGC provided. Reject unsupported
// bind flags, dimensions and device identity instead of silently copying.
FramePtr PrepareFrame(wgc::Direct3D11CaptureFrame frame,DxDevices& dx,Flow11& flow){
    const auto content=frame.ContentSize();
    if(content.Width!=int32_t(dx.srcW)||content.Height!=int32_t(dx.srcH)){
        frame.Close();throw WgcNeedsRecreate("WGC window content size changed");
    }
    auto surface=frame.Surface();
    auto access=surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    auto result=std::make_unique<CapturedFrame>();
    result->flowOwner=&flow;
    result->frame=std::move(frame);
    Check(access->GetInterface(IID_PPV_ARGS(&result->texture)),"WGC surface D3D11 texture query");
    D3D11_TEXTURE2D_DESC desc{};result->texture->GetDesc(&desc);
    if(desc.Width!=dx.srcW||desc.Height!=dx.srcH||
       desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||
       desc.SampleDesc.Count!=1||
       !(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE)){
        throw Error("WGC frame is not a directly sampleable native BGRA texture; strict zero-copy refused");
    }
    ComPtr<ID3D11Device> owner;
    result->texture->GetDevice(&owner);
    if(owner.Get()!=dx.dev11.Get()){
        throw Error("WGC texture belongs to a different D3D11 device; no copy permitted");
    }
    Check(dx.dev11->CreateShaderResourceView(result->texture.Get(),nullptr,&result->srv),
          "WGC direct texture SRV refused");
    D3D11_QUERY_DESC query{};query.Query=D3D11_QUERY_EVENT;
    Check(dx.dev11->CreateQuery(&query,&result->retirement),"WGC resource retirement fence");
    QueryPerformanceCounter(&result->capturedAt);
    return result;
}

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

// Each session owns the WGC pool, D3D11 textures, NvOFA and presenter.
// Recreating the session releases all resources before the next attempt.
SessionOutcome EngineSession(const Game& game,const TrackedGame& tracked){
    try {
        if(tracked.Exited())return SessionOutcome::Stop;
        // C++/WinRT initializes on this dedicated engine thread.
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        // Select the game window directly; unlike NvFBC this isn't a display crop.
        auto item=CaptureWgc::SelectWindow(game.window);
        const auto [sourceW,sourceH]=CaptureWgc::Size(item);
        OverlayWindow overlay;overlay.Init(game.window);
        MONITORINFOEXW monitorInfo{};monitorInfo.cbSize=sizeof(monitorInfo);
        UINT monitorHz=240;
        if(GetMonitorInfoW(MonitorFromWindow(game.window,MONITOR_DEFAULTTONEAREST),&monitorInfo)){
            DEVMODEW dm{};dm.dmSize=sizeof(dm);
            if(EnumDisplaySettingsW(monitorInfo.szDevice,ENUM_CURRENT_SETTINGS,&dm)&&dm.dmDisplayFrequency>=30)
                monitorHz=dm.dmDisplayFrequency;
        }
        DxDevices dx;dx.Init(overlay.hwnd,sourceW,sourceH,overlay.width,overlay.height);
        Flow11 flow;flow.Init(dx,kOpticalFlowGrid);
        CaptureWgc capture;capture.Init(std::move(item),dx.dev11.Get(),sourceW,sourceH);
        Status(L"Running");
        LARGE_INTEGER freq{},last{},now{};
        QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&last);
        LARGE_INTEGER previousCaptureAt{},previousPresent{};
        double sourcePeriod=1.0/60.0;
        int stableDynamicFactor=1,proposedDynamicFactor=1,proposalSamples=0;
        FramePtr current,previous;
        std::vector<FramePtr> retired;
        FrameLifetimeGuard lifetime{dx,current,previous,retired};
        bool flowReady=false,realFramePending=false,flowPending=false;
        bool wasFocused=false;
        int resumeWarmup=0;
        // No capture worker or D3D9Ex synchronization. A WGC event wakes this
        // same render thread and the newest WGC GPU texture is used directly.
        while(g_running.load(std::memory_order_acquire)){
            // Defer WGC frame Close and NvOFA unregister until the GPU no longer
            // references the captured texture. Never block the hot render loop.
            for(size_t i=0;i<retired.size();){
                const HRESULT r=dx.ctx11->GetData(retired[i]->retirement.Get(),nullptr,0,
                                                   D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if(r==S_OK){retired[i]->Close();retired.erase(retired.begin()+i);}
                else if(r==S_FALSE)++i;
                else Check(r,"WGC retired texture fence poll");
            }
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            if(tracked.Exited()){
                g_running.store(false,std::memory_order_release);break;
            }
            DWORD foregroundPid=0;
            HWND foreground=GetForegroundWindow();
            if(foreground)GetWindowThreadProcessId(foreground,&foregroundPid);
            const bool focused=!IsIconic(game.window)&&foregroundPid==game.pid;
            if(focused&&overlay.SyncToMonitor(game.window))
                dx.ResizeOutput(overlay.width,overlay.height);
            overlay.SetVisible(focused);
            if(!focused){
                wasFocused=false;flowReady=false;realFramePending=false;flowPending=false;
                // Return unread WGC frames promptly while unfocused.
                if(auto drop=capture.DrainLatest())drop.Close();
                Sleep(8);continue;
            }
            if(!wasFocused){
                wasFocused=true;resumeWarmup=2;previousPresent={};
                QueryPerformanceCounter(&last);
                // Do not extrapolate from frames captured during Alt+Tab.
                if(current){
                    dx.ctx11->End(current->retirement.Get());
                    retired.push_back(std::move(current));
                }
                if(previous){
                    dx.ctx11->End(previous->retirement.Get());
                    retired.push_back(std::move(previous));
                }
                previousCaptureAt={};
            }
            FramePtr next;
            if(auto wgcFrame=capture.DrainLatest())
                next=PrepareFrame(std::move(wgcFrame),dx,flow);
            if(next){
                if(previous){
                    dx.ctx11->End(previous->retirement.Get());
                    retired.push_back(std::move(previous));
                }
                previous=std::move(current);
                current=std::move(next);
                const auto capturedAt=current->capturedAt;
                if(previousCaptureAt.QuadPart && previous){
                    const double observed=double(capturedAt.QuadPart-previousCaptureAt.QuadPart)/double(freq.QuadPart);
                    if(observed>1.0/400.0 && observed<1.0/15.0)
                        sourcePeriod=.90*sourcePeriod+.10*observed;
                }
                previousCaptureAt=capturedAt;last=capturedAt;
                flowReady=false;realFramePending=true;
                flowPending=(previous && resumeWarmup==0);
                if(resumeWarmup>0)--resumeWarmup;
            }
            if(!current){
                const DWORD wake=MsgWaitForMultipleObjectsEx(1,&capture.arrivalEvent,4,
                    QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if(wake!=WAIT_OBJECT_0 && wake!=WAIT_OBJECT_0+1 && wake!=WAIT_TIMEOUT)
                    throw Error("WGC arrival wait failed",GetLastError());
                continue;
            }
            QueryPerformanceCounter(&now);
            const double elapsed=double(now.QuadPart-last.QuadPart)/double(freq.QuadPart);
            const double sincePresent=previousPresent.QuadPart?
                double(now.QuadPart-previousPresent.QuadPart)/double(freq.QuadPart):1.0;
            const int mode=std::clamp(g_fgMode.load(std::memory_order_relaxed),0,6);
            const int configuredTarget=g_targetHz.load(std::memory_order_relaxed);
            const int target=std::clamp(configuredTarget==0?int(monitorHz):configuredTarget,30,1000);
            const int measuredFactor=std::clamp(int(std::round(double(target)*sourcePeriod)),1,6);
            if(mode==0 && current->capturedAt.QuadPart==last.QuadPart && realFramePending){
                if(measuredFactor!=stableDynamicFactor){
                    if(measuredFactor==proposedDynamicFactor)++proposalSamples;
                    else {proposedDynamicFactor=measuredFactor;proposalSamples=1;}
                    if(proposalSamples>=3){stableDynamicFactor=measuredFactor;proposalSamples=0;}
                }else proposalSamples=0;
            }
            const int factor=(mode==0)?stableDynamicFactor:mode;
            const double step=(mode==0)?
                std::max(sourcePeriod/double(factor),1.0/double(target)):
                sourcePeriod/double(std::max(factor,1));
            if(!realFramePending&&elapsed>sourcePeriod*1.65){
                // Stop predicting across missing real frames.
                const DWORD wake=MsgWaitForMultipleObjectsEx(1,&capture.arrivalEvent,2,
                    QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if(wake==WAIT_FAILED)throw Error("WGC stalled wait failed",GetLastError());
                continue;
            }
            const bool doPresent=realFramePending||sincePresent>=step;
            if(doPresent){
                DWORD ready=WaitForSingleObject(dx.frameReady,0);
                if(ready==WAIT_TIMEOUT){
                    HANDLE handles[2]{dx.frameReady,capture.arrivalEvent};
                    const DWORD wake=MsgWaitForMultipleObjectsEx(2,handles,2,
                        QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                    if(wake==WAIT_OBJECT_0)ready=WAIT_OBJECT_0;
                    else if(wake==WAIT_OBJECT_0+1 || wake==WAIT_OBJECT_0+2 || wake==WAIT_TIMEOUT)
                        continue;
                    else throw Error("DXGI/WGC event wait failed",GetLastError());
                }
                if(ready!=WAIT_OBJECT_0)throw Error("DXGI frame latency wait failed",GetLastError());
                const float prediction=(realFramePending||factor<=1||!flowReady)?0.f:
                    float(std::clamp(elapsed/sourcePeriod,0.0,1.25));
                dx.Draw(current->srv.Get(),previous?previous->srv.Get():current->srv.Get(),
                        flowReady&&factor>1?flow.motionSRV.Get():nullptr,prediction,kOpticalFlowGrid);
                const bool submittedReal=realFramePending;
                realFramePending=false;
                if(submittedReal&&flowPending){
                    if(factor>1){
                        // Register WGC's ORIGINAL frame textures only when FG
                        // needs optical flow; Off mode skips this extra work.
                        if(!current->ofInput)current->ofInput=flow.Register(current->texture.Get());
                        if(!previous->ofInput)previous->ofInput=flow.Register(previous->texture.Get());
                        flow.Calculate(current->ofInput,previous->ofInput);
                        flowReady=true;
                    }
                    flowPending=false;
                }
                QueryPerformanceCounter(&previousPresent);
            }else{
                // Event-driven wait when the next prediction isn't yet due.
                const DWORD millis=DWORD(std::clamp((step-sincePresent)*1000.,0.,2.));
                if(millis){
                    const DWORD wake=MsgWaitForMultipleObjectsEx(1,&capture.arrivalEvent,millis,
                        QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                    if(wake==WAIT_FAILED)throw Error("WGC/frame pacing wait failed",GetLastError());
                }else SwitchToThread();
            }
        }
        g_running.store(false,std::memory_order_release);
        overlay.SetVisible(false);
        capture.Close();
        // FrameLifetimeGuard flushes the GPU and closes retained WGC frames,
        // even on exception paths, before NvOFA and D3D11 teardown.
        Status(L"Stopped");
    }catch(const WgcNeedsRecreate&){
        g_running.store(false,std::memory_order_release);
        if(!tracked.Exited()&&!g_userStopRequested.load())return SessionOutcome::Recreate;
    }catch(const winrt::hresult_error& e){
        if(!tracked.Exited()&&!g_userStopRequested.load())
            Status(L"FG error: WGC HRESULT "+Wide(winrt::to_string(e.message())));
        g_running.store(false,std::memory_order_release);
    }catch(const std::exception& e){
        if(!tracked.Exited()&&!g_userStopRequested.load())
            Status(L"FG error: "+Wide(e.what()));
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
            Status(L"Source error: WGC window size repeatedly changed. "
                   L"Full capture restart failed; check window/display mode.");
            break;
        }
        // Allow transient display-mode/topology changes to settle before
        // rebuilding WGC and NvOFA for the selected game window.
        for(unsigned i=0;i<10&&!g_userStopRequested.load()&&!tracked.Exited();++i)
            Sleep(10);
    }
    g_running.store(false,std::memory_order_release);
    g_lastGameExited.store(tracked.Exited(),std::memory_order_release);
    // Publish completion only AFTER all capture sessions have
    // returned; never treat a transient WGC recreation as a full stop.
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
        for(const wchar_t* mode:{L"Off",L"x2",L"x3",L"x4"})
            SendMessageW(g_multi,CB_ADDSTRING,0,(LPARAM)mode);
        SendMessageW(g_multi,CB_SETCURSEL,0,0);
        UiLabel(hwnd,L"NvOFA FAST 4x4 | cost OFF",217,225,245,20,true);
        g_dynamicTarget=CreateWindowW(L"EDIT",L"240",WS_CHILD|ES_NUMBER|ES_AUTOHSCROLL,
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
        // The timer is a fallback for a missed/late worker completion message.
        // It never enables Start while WGC is still restarting or tearing down.
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
        case kRefresh:if(!g_running.load())RefreshGames();break;
        case kMultiplier:
            if(HIWORD(wp)==CBN_SELCHANGE){
                const auto value=SendMessageW(g_multi,CB_GETCURSEL,0,0);
                if(value!=CB_ERR)g_fgMode.store(int(value)+1);
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
            g_fgMode.store(chosen==CB_ERR?1:int(chosen)+1);
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
        // A session can temporarily stop for WGC recovery. Only the final
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
    o.lpszClassName=L"FGWGCOverlay";o.hCursor=nullptr;
    o.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    if(!RegisterClassW(&o))return 1;
    WNDCLASSW c{};c.hInstance=instance;c.lpfnWndProc=ControlProc;
    c.lpszClassName=L"FGWGCControl";c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
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
