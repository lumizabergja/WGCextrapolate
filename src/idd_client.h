#pragma once
// App-owned NT shared GPU texture. Driver can write only while the user starts IddCx mode.
// Key 0 = producer may write; key 1 = consumer may read. One-slot bounded mailbox.
struct IddClient {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    HANDLE shared{};
    HMONITOR source{};
    UINT w{},h{};
    ULONGLONG firstPoll{};
    bool delivered{};
    ~IddClient(){if(shared)CloseHandle(shared);}
    void CheckSource(HWND game){
        RECT c{};POINT p{};
        MONITORINFO mi{sizeof(mi)};
        if(!GetClientRect(game,&c)||!ClientToScreen(game,&p)||
           !GetMonitorInfoW(source,&mi))throw DisplayChanged{};
        if(MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST)!=source||
           p.x!=mi.rcMonitor.left||p.y!=mi.rcMonitor.top||
           c.right!=LONG(w)||c.bottom!=LONG(h)||
           mi.rcMonitor.right-mi.rcMonitor.left!=LONG(w)||
           mi.rcMonitor.bottom-mi.rcMonitor.top!=LONG(h))
            throw Error("IddCx requires game fullscreen borderless on FG virtual display");
    }
    void Init(ID3D11Device* device,HWND game,HMONITOR output,UINT width,UINT height){
        source=MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST);w=width;h=height;
        if(source==output)throw Error("Move game to the FG virtual display; keep physical monitor primary");
        CheckSource(game);
        DWORD session=0;
        if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)||
           session!=WTSGetActiveConsoleSessionId())
            throw Error("IddCx requires the active local console session");
        HANDLE token{};
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw Error("Read user SID",GetLastError());
        DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
        std::vector<unsigned char> buf(bytes);
        if(!bytes||!GetTokenInformation(token,TokenUser,buf.data(),bytes,&bytes)){
            CloseHandle(token);throw Error("Read user SID",GetLastError());
        }
        CloseHandle(token);
        LPWSTR sid{};
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid,&sid))
            throw Error("Convert user SID",GetLastError());
        std::wstring sddl=L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;";
        sddl+=sid;sddl+=L")";LocalFree(sid);
        PSECURITY_DESCRIPTOR sd{};
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&sd,nullptr))
            throw Error("Create shared texture ACL",GetLastError());
        struct SdLease{PSECURITY_DESCRIPTOR p;~SdLease(){if(p)LocalFree(p);}} guard{sd};
        SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        Check(device->CreateTexture2D(&d,nullptr,&texture),"Create IddCx mailbox");
        Check(texture.As(&mutex),"IddCx mailbox keyed mutex");
        ComPtr<IDXGIResource1> resource;Check(texture.As(&resource),"IddCx mailbox NT resource");
        std::wstring name=L"Global\\FGIddBridgeV1-"+std::to_wstring(session);
        Check(resource->CreateSharedHandle(&sa,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,name.c_str(),&shared),
            "Create global IddCx mailbox (run elevated; restart driver if name already exists)");
    }
    bool Grab(ID3D11DeviceContext* ctx,HWND game,ID3D11Texture2D* dest,UINT timeout,LARGE_INTEGER& time){
        CheckSource(game);
        if(!firstPoll)firstPoll=GetTickCount64();
        HRESULT hr=mutex->AcquireSync(1,timeout);
        if(hr==WAIT_TIMEOUT){
            if(!delivered&&GetTickCount64()-firstPoll>5000)
                throw Error("No IddCx GPU frames: check signed driver, virtual display, GPU adapter and BGRA8 mode");
            return false;
        }
        if(hr!=S_OK)throw Error("IddCx mailbox acquire",hr);
        QueryPerformanceCounter(&time); // receive timestamp, not producer present timestamp
        ctx->CopyResource(dest,texture.Get());
        ctx->Flush();
        hr=mutex->ReleaseSync(0);
        if(hr!=S_OK)throw Error("IddCx mailbox release",hr);
        delivered=true;return true;
    }
};
