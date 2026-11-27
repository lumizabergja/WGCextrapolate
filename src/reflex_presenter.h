#pragma once
// Official NVAPI, only FG's D3D11 device. No CS2 device or process access.
// Keep the existing DXGI clock as sole explicit frame limiter.
class FgReflex {
    IUnknown* device{};
    bool initialized{},enabled{},attempted{};
    NvU64 frame{};
public:
    bool Begin(IUnknown* d,bool requested){
        if(!initialized){
            if(attempted)return false;
            attempted=true;
            initialized=NvAPI_Initialize()==NVAPI_OK;
            if(!initialized)return false;
            device=d;
        }
        if(!requested){Disable();return false;}
        if(!enabled){
            NV_SET_SLEEP_MODE_PARAMS_V1 p{};p.version=NV_SET_SLEEP_MODE_PARAMS_VER1;
            p.bLowLatencyMode=1;p.bLowLatencyBoost=1;p.bUseMarkersToOptimize=0;p.minimumIntervalUs=0;
            enabled=NvAPI_D3D_SetSleepMode(device,reinterpret_cast<NV_SET_SLEEP_MODE_PARAMS*>(&p))==NVAPI_OK;
            if(!enabled)return false;
        }
        // Optional comparison path; the existing waitable swapchain may make
        // sleep redundant, so leave driver sleeping opt-in.
        wchar_t opt[4]{};
        if(GetEnvironmentVariableW(L"FG_REFLEX_SLEEP",opt,4)==1&&opt[0]==L'1')
            NvAPI_D3D_Sleep(device);
        ++frame;
        Marker(RENDERSUBMIT_START);
        return true;
    }
    void Marker(NV_LATENCY_MARKER_TYPE type){
        if(!enabled)return;
        NV_LATENCY_MARKER_PARAMS_V1 p{};p.version=NV_LATENCY_MARKER_PARAMS_VER1;
        p.frameID=frame;p.markerType=type;
        NvAPI_D3D_SetLatencyMarker(device,reinterpret_cast<NV_LATENCY_MARKER_PARAMS*>(&p));
    }
    void Disable(){
        if(!enabled)return;
        NV_SET_SLEEP_MODE_PARAMS_V1 p{};p.version=NV_SET_SLEEP_MODE_PARAMS_VER1;
        NvAPI_D3D_SetSleepMode(device,reinterpret_cast<NV_SET_SLEEP_MODE_PARAMS*>(&p));enabled=false;
    }
    ~FgReflex(){Disable();if(initialized)NvAPI_Unload();}
};
