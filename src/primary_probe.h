#pragma once
// Included inside main.cpp's anonymous namespace, after TrackedGame.
// D3DKMT public ABI subset (d3dkmthk.h). No WDK or driver install needed.
struct PrimaryOpenAdapter { HDC dc; UINT adapter; LUID luid; UINT source; };
struct PrimaryGetHandle { UINT adapter; UINT source; UINT shared; };
struct PrimaryCloseAdapter { UINT adapter; };
static_assert(sizeof(PrimaryOpenAdapter)==(sizeof(void*)==8?24:20));
static_assert(sizeof(PrimaryGetHandle)==12);
using PrimaryOpenFn=LONG (WINAPI*)(PrimaryOpenAdapter*);
using PrimaryGetFn=LONG (WINAPI*)(PrimaryGetHandle*);
using PrimaryCloseFn=LONG (WINAPI*)(PrimaryCloseAdapter*);
struct PrimaryAdapterLease {
    HDC dc{}; PrimaryOpenAdapter opened{}; PrimaryCloseFn close{};
    ~PrimaryAdapterLease(){if(opened.adapter&&close){PrimaryCloseAdapter a{opened.adapter};close(&a);}if(dc)DeleteDC(dc);}
};
// This is deliberately a diagnostic, not a replacement for DXGI capture.
// No presenter is created: raw primary access has no verified capture exclusion.
void PrimaryProbeSession(const TrackedGame& tracked){
    const HWND game=tracked.game.window;
    // Wait for launcher to return foreground focus before selecting the display.
    const ULONGLONG focusStart=GetTickCount64();
    while(g_running.load()&&!tracked.Exited()&&GetForegroundWindow()!=game&&GetTickCount64()-focusStart<3000)Sleep(15);
    if(!g_running.load()||tracked.Exited())return;
    if(GetForegroundWindow()!=game)throw Error("Focus selected game before probe");
    const auto module=GetModuleHandleW(L"gdi32.dll");
    if(!module)throw Error("gdi32 unavailable");
    const auto open=reinterpret_cast<PrimaryOpenFn>(GetProcAddress(module,"D3DKMTOpenAdapterFromHdc"));
    const auto get=reinterpret_cast<PrimaryGetFn>(GetProcAddress(module,"D3DKMTGetSharedPrimaryHandle"));
    const auto close=reinterpret_cast<PrimaryCloseFn>(GetProcAddress(module,"D3DKMTCloseAdapter"));
    if(!open||!get||!close)throw Error("D3DKMT exports unavailable");
    MONITORINFOEXW mi{};mi.cbSize=sizeof(mi);
    const HMONITOR monitor=MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST);
    if(!GetMonitorInfoW(monitor,&mi))throw Error("Probe monitor unavailable");
    PrimaryAdapterLease lease;lease.close=close;
    lease.dc=CreateDCW(L"DISPLAY",mi.szDevice,nullptr,nullptr);
    if(!lease.dc)throw Error("Create display DC",GetLastError());
    lease.opened.dc=lease.dc;
    LONG nt=open(&lease.opened);
    if(nt<0||!lease.opened.adapter)throw Error("D3DKMTOpenAdapterFromHdc",nt);
    PrimaryGetHandle primary{lease.opened.adapter,lease.opened.source,0};
    nt=get(&primary);
    if(nt<0||!primary.shared)throw Error("D3DKMT shared primary unavailable",nt);
    ComPtr<IDXGIFactory1> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Probe adapter enumeration");
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> candidate;HRESULT hr=factory->EnumAdapters1(i,&candidate);
        if(hr==DXGI_ERROR_NOT_FOUND)break;Check(hr,"Probe adapter enumeration");
        DXGI_ADAPTER_DESC1 d{};Check(candidate->GetDesc1(&d),"Probe adapter description");
        if(d.AdapterLuid.LowPart==lease.opened.luid.LowPart&&d.AdapterLuid.HighPart==lease.opened.luid.HighPart){adapter=candidate;break;}
    }
    if(!adapter)throw Error("No matching D3D11 adapter for D3DKMT LUID");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Probe D3D11 device");
    // The global KMT handle may NOT be a D3D-compatible legacy shared handle.
    // Try only the returned handle, never guessed handles or private escapes.
    ComPtr<ID3D11Texture2D> surface;
    HRESULT hr=device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(primary.shared)),IID_PPV_ARGS(&surface));
    Check(hr,"Primary handle exists but D3D11 OpenSharedResource failed");
    D3D11_TEXTURE2D_DESC desc{};surface->GetDesc(&desc);
    if(desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM&&desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_B8G8R8X8_UNORM)
        throw Error("Primary opened but unsupported pixel format");
    if(desc.SampleDesc.Count!=1||desc.ArraySize!=1||desc.Width<32||desc.Height<32)
        throw Error("Primary opened but unsupported surface dimensions");
    // Probe only: copy one 16x16 tile at five locations into a tiny staging atlas.
    // Never store pixels; compare a checksum to see whether the surface changes.
    D3D11_TEXTURE2D_DESC sd{};sd.Width=80;sd.Height=16;sd.MipLevels=1;sd.ArraySize=1;
    sd.Format=desc.Format;sd.SampleDesc.Count=1;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&sd,nullptr,&staging),"Probe sample staging");
    unsigned samples=0,changes=0;unsigned long long prior=0;
    const ULONGLONG start=GetTickCount64();
    while(g_running.load()&&!tracked.Exited()&&GetTickCount64()-start<8000){
        if(GetForegroundWindow()!=game){Sleep(30);continue;}
        if(MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST)!=monitor)throw Error("Probe monitor changed");
        PrimaryGetHandle current{lease.opened.adapter,lease.opened.source,0};
        nt=get(&current);if(nt<0||current.shared!=primary.shared)throw Error("Primary handle changed during probe",nt);
        RECT rc{};POINT p{};
        if(!GetClientRect(game,&rc)||!ClientToScreen(game,&p))break;
        const LONG left=p.x-mi.rcMonitor.left,top=p.y-mi.rcMonitor.top;
        const LONG width=rc.right,height=rc.bottom;
        if(left<0||top<0||width<64||height<64||left+width>LONG(desc.Width)||top+height>LONG(desc.Height))
            throw Error("Game client not fully inside primary surface");
        const UINT fx[5]={25,75,50,25,75},fy[5]={25,25,50,75,75};
        for(UINT i=0;i<5;i++){
            UINT x=UINT(left+(width-16)*fx[i]/100),y=UINT(top+(height-16)*fy[i]/100);
            D3D11_BOX box{x,y,0,x+16,y+16,1};
            context->CopySubresourceRegion(staging.Get(),0,16*i,0,0,surface.Get(),0,&box);
        }
        D3D11_MAPPED_SUBRESOURCE map{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Probe sample map");
        unsigned long long hash=14695981039346656037ull;
        for(UINT y=0;y<16;y++){
            const auto* row=static_cast<const unsigned char*>(map.pData)+size_t(y)*map.RowPitch;
            for(UINT x=0;x<80;x++)for(UINT c=0;c<3;c++){hash^=row[4*x+c];hash*=1099511628211ull;}
        }
        context->Unmap(staging.Get(),0);
        if(samples&&hash!=prior)++changes;prior=hash;++samples;
        Sleep(8);
    }
    wchar_t result[400]{};
    std::swprintf(result,400,L"D3DKMT handle and D3D11 texture opened: %u x %u.\nSamples: %u; changed samples: %u.\n%ls\nThis does not establish frame cadence, latency, or capture exclusion.",desc.Width,desc.Height,samples,changes,
        changes?L"Surface changes observed. Next: verify game frame identity and synchronization.":L"No changes observed. Not a usable live capture result yet.");
    Status(std::wstring(L"Probe result: ")+result);
}
void PrimaryProbeEngine(Game game){
    TrackedGame tracked(game);
    try{PrimaryProbeSession(tracked);}catch(const std::exception& e){Status(L"Source error: "+Wide(e.what()));}
    g_running.store(false,std::memory_order_release);
    g_lastGameExited.store(tracked.Exited(),std::memory_order_release);
    g_engineFinished.store(true,std::memory_order_release);
    if(g_control)PostMessageW(g_control,WM_FG_STOPPED,tracked.Exited()?1u:0u,0);
}
