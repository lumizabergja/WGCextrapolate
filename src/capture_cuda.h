#pragma once

template<class T> constexpr DWORD fbcVersion(unsigned revision) {
    return DWORD(sizeof(T)) | (revision<<16) | 0x70000000u;
}

struct CaptureFrame {
    CUdeviceptr pixels{};
    CUevent captured{};
    UINT width{},height{},pitchPixels{};
    uint64_t sequence{};
};

class CaptureCuda {
    struct Setup {
        DWORD version{},flags{}; void* cursor{}; DWORD format{},reserved[61]{}; void* pointers[31]{};
    };
    struct Grab {
        DWORD version{},flags{}; CUdeviceptr buffer{}; NvFBCFrameGrabInfo* info{};
        DWORD waitMs{},reserved[61]{}; void* pointers[30]{};
    };
    static_assert(sizeof(Setup)==512 && sizeof(Grab)==512 && offsetof(Grab,waitMs)==24);
    static_assert(sizeof(NvFBCFrameGrabInfo)==108 && sizeof(NvFBCCreateParams)==512);
    const CudaDriver& cu;
    SystemLibrary fbc{L"NvFBC64.dll"};
    void* object{};
    NVFBCRESULT (WINAPI *grab)(void*,Grab*){};
    NVFBCRESULT (WINAPI *release)(void*){};
    DWORD capacity{},pid{};
    uint64_t lastSequence{};
    static void check(NVFBCRESULT status, const char* operation) {
        if (status!=NVFBC_SUCCESS)
            throw std::runtime_error(std::string(operation)+" ("+std::to_string(int(status))+")");
    }
public:
    std::array<CaptureFrame,kNumCapture> frames{};
    CUcontext context{};
    explicit CaptureCuda(const CudaDriver& driver) : cu(driver) {}
    void Init(DWORD target, IDXGIAdapter* adapter) {
        pid=target;
        cu.check(cu.init(0),"Initialize CUDA");
        using StatusFn=NVFBCRESULT(WINAPI*)(NvFBCStatusEx*);
        struct PidStatus { DWORD version, reserved0[2], flags, reserved[44]; };
        PidStatus pidStatus{}; pidStatus.version=fbcVersion<PidStatus>(1);
        NvFBCStatusEx status{}; status.dwVersion=fbcVersion<NvFBCStatusEx>(2);
        status.pPrivateData=&pidStatus; status.dwPrivateDataSize=sizeof(pidStatus);
        check(fbc.symbol<StatusFn>("NvFBC_GetStatusEx")(&status),"NvFBC PID status");
        if (!status.bIsCapturePossible)
            throw std::runtime_error("NvFBC reports capture disabled. Enable NvFBC before starting FG.");
        if (auto flags=fbc.optional<decltype(&NvFBC_SetGlobalFlags)>("NvFBC_SetGlobalFlags"))
            flags(NVFBC_GLOBAL_FLAGS_NO_INITIAL_REFRESH);

        // Use the same 0x70 PID session request as the working CUDA capture probe.
        struct PidSelection {DWORD version, enable, pid, reserved[47];};
        static_assert(fbcVersion<PidSelection>(1)==0x700100c8);
        PidSelection selection{}; selection.version=fbcVersion<PidSelection>(1);
        selection.enable=1; selection.pid=target;
        DWORD compatibility[4]={0xaef57ac5u,0x401d1a39u,0x1b856bbeu,0x9ed0cebau};
        NvFBCCreateParams cp{}; cp.dwVersion=fbcVersion<NvFBCCreateParams>(2);
        cp.dwInterfaceType=0x1007;
        cp.pPrivateData=compatibility; cp.dwPrivateDataSize=sizeof(compatibility);
        cp.pPrivateData2=&selection; cp.dwPrivateData2Size=sizeof(selection);
        using Create=NVFBCRESULT(WINAPI*)(NvFBCCreateParams*);
        check(fbc.symbol<Create>("NvFBC_CreateEx")(&cp),"Create NvFBC CUDA PID session");
        object=cp.pNvFBC;
        if (!object) throw Error("NvFBC returned an empty session");
        auto vtable=*static_cast<void***>(object);
        using Max=NVFBCRESULT(WINAPI*)(void*,DWORD*);
        using Configure=NVFBCRESULT(WINAPI*)(void*,Setup*);
        grab=reinterpret_cast<decltype(grab)>(vtable[2]);
        release=reinterpret_cast<decltype(release)>(vtable[5]);
        cu.check(cu.getContext(&context),"Get NvFBC CUDA context");
        if (!context) throw Error("NvFBC did not create a CUDA context");
        CUdevice captureGpu{},displayGpu{};
        cu.check(cu.ctxDevice(&captureGpu),"Get capture GPU");
        cu.check(cu.d3dDevice(&displayGpu,adapter),"Identify DXGI presenter GPU");
        if (captureGpu!=displayGpu) throw Error("Capture and display are on different GPUs");
        check(reinterpret_cast<Max>(vtable[0])(object,&capacity),"Get NvFBC maximum allocation");
        if (!capacity) throw Error("NvFBC returned an empty capture allocation");
        for (auto& frame:frames) {
            cu.check(cu.allocate(&frame.pixels,capacity),"Allocate native NvFBC capture slot");
            frame.captured=cu.event();
        }
        Setup setup{}; setup.version=fbcVersion<Setup>(1);
        check(reinterpret_cast<Configure>(vtable[1])(object,&setup),"Configure NvFBC CUDA ARGB");
    }
    bool GrabSlot(int index) {
        auto& slot=frames[index];
        NvFBCFrameGrabInfo info{};
        Grab request{}; request.version=fbcVersion<Grab>(1);
        request.flags=0x10; // NVFBC_TOCUDA_WAIT_WITH_TIMEOUT
        request.waitMs=16; request.buffer=slot.pixels; request.info=&info;
        auto result=grab(object,&request);
        if (result==NVFBC_ERROR_INVALIDATED_SESSION || info.bMustRecreate)
            throw NvFbcNeedsRecreate(NVFBC_ERROR_INVALIDATED_SESSION);
        if (result==NVFBC_ERROR_PROTECTED_CONTENT || info.bProtectedContent)
            throw Error("NvFBC protected content",result);
        check(result,"Grab NvFBC CUDA frame");
        if (!info.dwWidth || !info.dwHeight || (info.dwSourcePID && info.dwSourcePID!=pid)) return false;
        const uint64_t row=uint64_t(info.dwWidth)*4, pitch=uint64_t(info.dwBufferWidth)*4;
        if (info.dwWidth>16384 || info.dwHeight>16384 || pitch<row || row>capacity ||
            (info.dwHeight>1 && pitch>(capacity-row)/(info.dwHeight-1)))
            throw Error("Invalid NvFBC capture dimensions or pitch");
        slot.width=info.dwWidth; slot.height=info.dwHeight; slot.pitchPixels=info.dwBufferWidth;
        slot.sequence=++lastSequence;
        cu.diagnosticSync(nullptr,"Diagnostic: NvFBC capture");
        cu.check(cu.recordEvent(slot.captured,nullptr),"Publish captured CUDA frame");
        return true;
    }
    ~CaptureCuda() {
        // The producer has left its context and the consumer has drained its streams.
        if (context) cu.syncStream(nullptr);
        for (auto& frame:frames) {
            if (frame.captured) cu.destroyEvent(frame.captured);
            if (frame.pixels) cu.freeMemory(frame.pixels);
        }
        if (object && release) release(object);
    }
};
