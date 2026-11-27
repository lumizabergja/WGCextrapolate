#pragma once
// Official NVAPI, exclusively for FG's own D3D11 device.
// No frame-rate limit in NVAPI: DXGI/display clock remains the limiter.
enum class ReflexState {Idle,Off,Active,ModeRejected,InitFailed,SleepRejected,MarkersRejected,ActiveNoSleep};
class FgReflex {
    IUnknown* device{};
    bool initialized{},enabled{},attempted{},modeAttempted{},sleepFailed{},markerFailed{};
    NvU64 frame{};
    ReflexState state=ReflexState::Idle;
public:
    ReflexState State()const{return state;}
    void Begin(IUnknown* d,bool requested){
        if(!requested){Disable();state=ReflexState::Off;return;}
        if(!initialized){
            if(!attempted){attempted=true;initialized=NvAPI_Initialize()==NVAPI_OK;device=d;}
            if(!initialized){state=ReflexState::InitFailed;return;}
        }
        if(!enabled){
            if(modeAttempted){state=ReflexState::ModeRejected;return;}
            modeAttempted=true;
            NV_SET_SLEEP_MODE_PARAMS_V1 p{};p.version=NV_SET_SLEEP_MODE_PARAMS_VER1;
            p.bLowLatencyMode=1;p.bLowLatencyBoost=1;p.bUseMarkersToOptimize=0;p.minimumIntervalUs=0;
            enabled=NvAPI_D3D_SetSleepMode(device,reinterpret_cast<NV_SET_SLEEP_MODE_PARAMS*>(&p))==NVAPI_OK;
            if(!enabled){state=ReflexState::ModeRejected;return;}
        }
        // Default: use actual driver sleep before FG rendering. No second
        // explicit FPS cap. FG_REFLEX_SLEEP=0 permits a diagnostic A/B test.
        wchar_t opt[4]{};
        const bool noSleep=GetEnvironmentVariableW(L"FG_REFLEX_SLEEP",opt,4)==1&&opt[0]==L'0';
        if(!noSleep&&!sleepFailed)sleepFailed=NvAPI_D3D_Sleep(device)!=NVAPI_OK;
        state=sleepFailed?ReflexState::SleepRejected:(noSleep?ReflexState::ActiveNoSleep:ReflexState::Active);
        ++frame;
        Marker(RENDERSUBMIT_START);
    }
    void Marker(NV_LATENCY_MARKER_TYPE type){
        if(!enabled)return;
        if(markerFailed){if(state==ReflexState::Active)state=ReflexState::MarkersRejected;return;}
        NV_LATENCY_MARKER_PARAMS_V1 p{};p.version=NV_LATENCY_MARKER_PARAMS_VER1;
        p.frameID=frame;p.markerType=type;
        markerFailed=NvAPI_D3D_SetLatencyMarker(device,reinterpret_cast<NV_LATENCY_MARKER_PARAMS*>(&p))!=NVAPI_OK;
        if(markerFailed&&state==ReflexState::Active)state=ReflexState::MarkersRejected;
    }
    void Disable(){
        if(enabled){
            NV_SET_SLEEP_MODE_PARAMS_V1 p{};p.version=NV_SET_SLEEP_MODE_PARAMS_VER1;
            NvAPI_D3D_SetSleepMode(device,reinterpret_cast<NV_SET_SLEEP_MODE_PARAMS*>(&p));
        }
        enabled=false;modeAttempted=false;sleepFailed=false;markerFailed=false;state=ReflexState::Idle;
    }
    ~FgReflex(){Disable();if(initialized)NvAPI_Unload();}
};
