#pragma once

// Driver DLLs load only when the user starts a session.
class SystemLibrary {
    HMODULE module;
public:
    explicit SystemLibrary(const wchar_t* name) : module(LoadLibraryExW(name,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module) {
            std::wstring n(name);
            throw Error("Load " + std::string(n.begin(),n.end()), GetLastError());
        }
    }
    SystemLibrary(const SystemLibrary&)=delete;
    SystemLibrary& operator=(const SystemLibrary&)=delete;
    ~SystemLibrary() { FreeLibrary(module); }
    template<class T> T optional(const char* name) const {
        return reinterpret_cast<T>(GetProcAddress(module,name));
    }
    template<class T> T symbol(const char* name) const {
        auto entry=optional<T>(name);
        if (!entry) throw Error(std::string("Missing driver export ")+name,GetLastError());
        return entry;
    }
};

struct CudaDriver {
    SystemLibrary library{L"nvcuda.dll"};
    // The two-stage string expansion follows the official CUDA header's _v2 ABI names.
#define FG_DRIVER_STRING_IMPL(name) #name
#define FG_DRIVER_STRING(name) FG_DRIVER_STRING_IMPL(name)
#define FG_DRIVER(field,name) decltype(&name) field=library.symbol<decltype(&name)>(FG_DRIVER_STRING(name))
    FG_DRIVER(init,cuInit);
    FG_DRIVER(errorName,cuGetErrorName);
    FG_DRIVER(getContext,cuCtxGetCurrent);
    FG_DRIVER(pushContext,cuCtxPushCurrent);
    FG_DRIVER(popContext,cuCtxPopCurrent);
    FG_DRIVER(ctxDevice,cuCtxGetDevice);
    FG_DRIVER(d3dDevice,cuD3D11GetDevice);
    FG_DRIVER(allocate,cuMemAlloc);
    FG_DRIVER(freeMemory,cuMemFree);
    FG_DRIVER(createStream,cuStreamCreate);
    FG_DRIVER(createPriorityStream,cuStreamCreateWithPriority);
    FG_DRIVER(streamPriorities,cuCtxGetStreamPriorityRange);
    FG_DRIVER(syncStream,cuStreamSynchronize);
    FG_DRIVER(destroyStream,cuStreamDestroy);
    FG_DRIVER(createEvent,cuEventCreate);
    FG_DRIVER(recordEvent,cuEventRecord);
    FG_DRIVER(queryEvent,cuEventQuery);
    FG_DRIVER(waitEvent,cuStreamWaitEvent);
    FG_DRIVER(destroyEvent,cuEventDestroy);
    FG_DRIVER(loadModule,cuModuleLoadData);
    FG_DRIVER(unloadModule,cuModuleUnload);
    FG_DRIVER(getFunction,cuModuleGetFunction);
    FG_DRIVER(launchKernel,cuLaunchKernel);
    FG_DRIVER(registerD3D,cuGraphicsD3D11RegisterResource);
    FG_DRIVER(unregisterD3D,cuGraphicsUnregisterResource);
    FG_DRIVER(map,cuGraphicsMapResources);
    FG_DRIVER(unmap,cuGraphicsUnmapResources);
    FG_DRIVER(mappedPointer,cuGraphicsResourceGetMappedPointer);
    FG_DRIVER(createTexture,cuTexObjectCreate);
    FG_DRIVER(destroyTexture,cuTexObjectDestroy);
#undef FG_DRIVER
#undef FG_DRIVER_STRING
#undef FG_DRIVER_STRING_IMPL

    void check(CUresult status, const char* operation) const {
        if (status==CUDA_SUCCESS) return;
        const char* name=nullptr;
        errorName(status,&name);
        throw std::runtime_error(std::string(operation)+": "+(name?name:std::to_string(int(status))));
    }
    // Opt-in diagnostic only; no additional synchronization in normal mode.
    bool diagnosticEnabled() const {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"FG_CUDA_DIAGNOSTIC",value,8)==1 && value[0]==L'1';
    }
    void diagnosticSync(CUstream stream,const char* operation) const {
        if(diagnosticEnabled())check(syncStream(stream),operation);
    }
    bool complete(CUevent event) const {
        auto result=queryEvent(event);
        if (result==CUDA_ERROR_NOT_READY) return false;
        check(result,"Query CUDA completion");
        return true;
    }
    CUevent event() const {
        CUevent result=nullptr;
        check(createEvent(&result,CU_EVENT_DISABLE_TIMING),"Create CUDA event");
        return result;
    }
    void launch(CUfunction kernel, CUstream stream, UINT width, UINT height, void** args) const {
        constexpr unsigned blockX=32, blockY=8;
        check(launchKernel(kernel,(width+blockX-1)/blockX,(height+blockY-1)/blockY,1,
                           blockX,blockY,1,0,stream,args,nullptr),"Launch CUDA image kernel");
    }
};

struct CudaThreadContext {
    const CudaDriver& cu;
    CudaThreadContext(const CudaDriver& driver, CUcontext context) : cu(driver) {
        cu.check(cu.pushContext(context),"Bind shared NvFBC CUDA context");
    }
    ~CudaThreadContext() { CUcontext old{}; cu.popContext(&old); }
};
