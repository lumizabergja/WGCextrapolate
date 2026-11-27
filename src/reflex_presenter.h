#pragma once
// Official NVAPI, exclusively for FG's own D3D11 device.
// No frame-rate limit in NVAPI: DXGI/display clock remains the limiter.
enum class ReflexState {Idle,Off,Active,ModeRejected,InitFailed,SleepRejected,MarkersRejected,ActiveNoSleep};
class FgReflex {
    IUnknown* device{};
    bool initialized{},enabled{},attempted{},modeAttempted{},sleepFailed{},markerFailed{};
    NvU64 frame{};
    double sleepMs=0.;unsigned samples=0,cooldown=0;
    int sleepOverride=-1;
    ReflexState state=ReflexState::Idle;
public:
    ReflexState State()const{return state;}
    void Prepare(IUnknown* d,bool requested,bool urgent,double periodMs){
        if(!requested){Disable();state=ReflexState::Off;return;}
        if(!initialized){
            if(!attempted){attempted=true;initialized=NvAPI_Initialize()==NVAPI_OK;device=d;}
            if(!initialized){state=ReflexState::InitFailed;return;}
        }
        if(!enabled){
            if(modeAttempted){state=ReflexState::ModeRejected;return;}
            modeAttempted=true;
            wchar_t opt[4]{};const DWORD n=GetEnvironmentVariableW(L"FG_REFLEX_SLEEP",opt,4);
            sleepOverride=(n==1&&opt[0]==L'0')?0:((n==1&&opt[0]==L'1')?1:-1);
            NV_SET_SLEEP_MODE_PARAMS_V1 p{};p.version=NV_SET_SLEEP_MODE_PARAMS_VER1;
            p.bLowLatencyMode=1;p.bLowLatencyBoost=1;p.bUseMarkersToOptimize=0;p.minimumIntervalUs=0;
            enabled=NvAPI_D3D_SetSleepMode(device,reinterpret_cast<NV_SET_SLEEP_MODE_PARAMS*>(&p))==NVAPI_OK;
            if(!enabled){state=ReflexState::ModeRejected;return;}
        }
        // DXGI slot is already available. Avoid double pacing when the
        // driver sleep becomes expensive; periodically probe generated slots.
        bool useSleep=sleepOverride!=0&&!sleepFailed;
        if(sleepOverride<0){
            if(urgent&&samples&&sleepMs>periodMs*.12)useSleep=false;
            if(cooldown){--cooldown;useSleep=false;}
        }
        if(useSleep){
            LARGE_INTEGER begin{},end{},freq{};QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&begin);
            sleepFailed=NvAPI_D3D_Sleep(device)!=NVAPI_OK;
            QueryPerformanceCounter(&end);
            const double ms=double(end.QuadPart-begin.QuadPart)*1000./double(freq.QuadPart);
            sleepMs=samples?(.8*sleepMs+.2*ms):ms;++samples;
            if(sleepOverride<0&&ms>periodMs*.20)cooldown=30;
        }
        state=sleepFailed?ReflexState::SleepRejected:(useSleep?ReflexState::Active:ReflexState::ActiveNoSleep);
    }
    void Begin(){if(enabled){++frame;Marker(RENDERSUBMIT_START);}}
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
        enabled=false;modeAttempted=false;sleepFailed=false;markerFailed=false;state=ReflexState::Idle;sleepMs=0;samples=cooldown=0;
    }
    ~FgReflex(){Disable();if(initialized)NvAPI_Unload();}
};
