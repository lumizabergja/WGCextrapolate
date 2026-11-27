#pragma once
// Included inside FG implementation namespace; SDK/STL headers must be
// included at global scope in main.cpp.
// TensorRT runtime is loaded from a user-installed NVIDIA SDK; no runtime
// binary or weights are redistributed. All execution uses the capture context.
class AiDepth {
    struct Logger final:nvinfer1::ILogger {
        void log(Severity,const char*) noexcept override {}
    } logger;
    const CudaDriver& cu;
    CUcontext context{};
    std::thread worker;
    std::mutex workLock;
    std::condition_variable workWake;
    bool stopping{},queued{},submitted{};
    std::exception_ptr failure;
    struct Job {int index{};uint64_t sequence{};CUevent captured{};CUdeviceptr pixels{};UINT width{},height{},pitch{};};
    Job job;
    CUdeviceptr fixedOutput{};
    CUgraphExec graphExec{};
    bool graphAttempted{};
    HMODULE dll{};
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> execution;
    CUstream stream{};
    CUevent done{};
    CUdeviceptr input{},temporalRange{};
    uint64_t lastRangeSequence{};
    struct Slot {CUdeviceptr raw{},range{};uint64_t sequence{};};
    std::array<Slot,kNumCapture> slots{};
    int active=-1;
    uint64_t pending{};
    CUfunction prep{},rangeKernel{};
    bool opened=false;
    static constexpr UINT W=512,H=384;
    static bool dims(nvinfer1::Dims d,std::initializer_list<int> expected){
        if(d.nbDims!=int(expected.size()))return false;
        int i=0;for(int n:expected)if(d.d[i++]!=n)return false;return true;
    }
public:
    explicit AiDepth(const CudaDriver& driver,CUcontext ctx):cu(driver),context(ctx){}
    void Open(CUmodule module){
        if(opened)return;
        wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))throw Error("Find FG executable");
        const auto folder=std::filesystem::path(path).parent_path();
        std::ifstream file(folder/L"AI"/L"zipdepth_512x384.plan",std::ios::binary|std::ios::ate);
        if(!file)throw Error("AI depth engine missing: run Setup-AI-Depth.ps1 beside FG.exe");
        const auto size=file.tellg();
        if(size<=0||size>512ll*1024*1024)throw Error("Invalid ZipDepth engine size");
        std::vector<char> bytes(static_cast<size_t>(size));file.seekg(0);file.read(bytes.data(),static_cast<std::streamsize>(size));
        if(!file)throw Error("Read ZipDepth engine");
        wchar_t dllPath[32768]{};
        const DWORD n=GetEnvironmentVariableW(L"FG_TRT_DLL",dllPath,32768);
        if(!n||n>=32768){
            auto portable=folder/L"AI"/L"Runtime"/L"TensorRT-10.13.0.35"/L"lib"/L"nvinfer_10.dll";
            if(!std::filesystem::exists(portable))portable=folder/L"AI"/L"Runtime"/L"TensorRT-10.13.0.35"/L"bin"/L"nvinfer_10.dll";
            if(!std::filesystem::exists(portable)||portable.native().size()>=32768)throw Error("Launch using Start-FG.cmd");
            wcscpy_s(dllPath,32768,portable.c_str());
        }
        dll=LoadLibraryExW(dllPath,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!dll)throw Error("Load NVIDIA TensorRT runtime",GetLastError());
        using Create=void*(*)(void*,int32_t) noexcept;
        auto create=reinterpret_cast<Create>(GetProcAddress(dll,"createInferRuntime_INTERNAL"));
        if(!create)throw Error("Missing TensorRT runtime factory");
        runtime.reset(static_cast<nvinfer1::IRuntime*>(create(&logger,NV_TENSORRT_VERSION)));
        if(!runtime)throw Error("TensorRT SDK/runtime mismatch (requires matching 10.13 headers/runtime)");
        engine.reset(runtime->deserializeCudaEngine(bytes.data(),bytes.size()));
        if(!engine)throw Error("TensorRT engine incompatible with installed runtime/GPU; rebuild locally");
        using namespace nvinfer1;
        if(engine->getNbIOTensors()!=2 || engine->getTensorIOMode("image")!=TensorIOMode::kINPUT ||
           engine->getTensorIOMode("depth")!=TensorIOMode::kOUTPUT ||
           engine->getTensorDataType("image")!=DataType::kFLOAT || engine->getTensorDataType("depth")!=DataType::kFLOAT ||
           engine->getTensorFormat("image")!=TensorFormat::kLINEAR || engine->getTensorFormat("depth")!=TensorFormat::kLINEAR ||
           !dims(engine->getTensorShape("image"),{1,3,384,512}))
            throw Error("ZipDepth engine must use static RGB FP32 CHW input and linear FP32 depth output");
        const auto d=engine->getTensorShape("depth");
        if(!dims(d,{1,1,384,512})&&!dims(d,{1,384,512})&&!dims(d,{384,512}))
            throw Error("Unexpected ZipDepth output shape");
        execution.reset(engine->createExecutionContext());
        if(!execution)throw Error("Create TensorRT execution context");
        int least=0,highest=0;cu.check(cu.streamPriorities(&least,&highest),"Depth stream priorities");
        cu.check(cu.createPriorityStream(&stream,CU_STREAM_NON_BLOCKING,least),"Create low priority AI depth stream");
        done=cu.event();
        cu.check(cu.allocate(&input,3ull*W*H*sizeof(float)),"Allocate RGB tensor");
        cu.check(cu.allocate(&fixedOutput,uint64_t(W)*H*sizeof(float)),"Allocate fixed graph output");
        cu.check(cu.allocate(&temporalRange,2*sizeof(float)),"Allocate temporal calibration");
        for(auto& s:slots){
            cu.check(cu.allocate(&s.raw,uint64_t(W)*H*sizeof(float)),"Allocate depth tensor");
            cu.check(cu.allocate(&s.range,2*sizeof(float)),"Allocate depth calibration");
        }
        cu.check(cu.getFunction(&prep,module,"prepareDepthRGB"),"Load depth preprocessing");
        cu.check(cu.getFunction(&rangeKernel,module,"depthRange"),"Load depth calibration");
        opened=true;
        worker=std::thread([this]{Run();});
    }
    void Poll(){
        bool published=false;
        {std::lock_guard<std::mutex> g(workLock);if(failure)std::rethrow_exception(failure);published=submitted;}
        if(active>=0&&published&&cu.complete(done)){
            slots[active].sequence=pending;active=-1;
            std::lock_guard<std::mutex> g(workLock);submitted=false;
        }
    }
    // One outstanding job; render-thread slot ownership persists until
    // worker submission AND the GPU completion event have both completed.
    void Submit(int index,const CaptureFrame& frame){
        Poll();
        if(active>=0||slots[index].sequence==frame.sequence)return;
        slots[index].sequence=0;active=index;pending=frame.sequence;
        {std::lock_guard<std::mutex> g(workLock);job={index,frame.sequence,frame.captured,frame.pixels,frame.width,frame.height,frame.pitchPixels};queued=true;submitted=false;}
        workWake.notify_one();
    }
private:
    void Infer(const Job& j){
        cu.check(cu.waitEvent(stream,j.captured,0),"AI wait for capture");
        CUdeviceptr pixels=j.pixels;UINT sw=j.width,sh=j.height,pitch=j.pitch;
        void* args[]={&pixels,&pitch,&sw,&sh,&input};
        cu.launch(prep,stream,W,H,args);
        if(!graphAttempted){
            graphAttempted=true;
            if(!execution->setTensorAddress("image",reinterpret_cast<void*>(input))||
               !execution->setTensorAddress("depth",reinterpret_cast<void*>(fixedOutput))||
               !execution->enqueueV3(reinterpret_cast<cudaStream_t>(stream)))
                throw Error("TensorRT depth warmup failed");
            // Warm up fixed tensor addresses before capturing. Only the AI
            // worker waits; the display thread is never blocked by this wait.
            cu.check(cu.syncStream(stream),"Finish AI warmup");
            wchar_t opt[4]{};const bool disabled=GetEnvironmentVariableW(L"FG_AI_GRAPH",opt,4)==1&&opt[0]==L'0';
            if(!disabled&&cu.beginCapture(stream,CU_STREAM_CAPTURE_MODE_THREAD_LOCAL)==CUDA_SUCCESS){
                bool ok=execution->enqueueV3(reinterpret_cast<cudaStream_t>(stream));
                CUgraph graph{};CUresult end=cu.endCapture(stream,&graph);
                if(ok&&end==CUDA_SUCCESS&&graph){
                    CUgraphExec candidate{};
                    if(cu.graphInstantiate(&candidate,graph,0)==CUDA_SUCCESS)graphExec=candidate;
                }
                if(graph)cu.graphDestroy(graph);
            }
            // Warmup already produced this frame's result.
        }else if(graphExec){
            cu.check(cu.graphLaunch(graphExec,stream),"Replay TensorRT CUDA graph");
        }else if(!execution->enqueueV3(reinterpret_cast<cudaStream_t>(stream)))throw Error("TensorRT depth enqueue failed");
        auto& s=slots[j.index];
        cu.check(cu.copyDevice(s.raw,fixedOutput,uint64_t(W)*H*sizeof(float),stream),"Publish fixed graph depth");
        unsigned contiguous=lastRangeSequence&&j.sequence==lastRangeSequence+1?1u:0u;
        void* rangeArgs[]={&s.raw,&s.range,&temporalRange,&contiguous};
        lastRangeSequence=j.sequence;
        cu.check(cu.launchKernel(rangeKernel,1,1,1,1,1,1,0,stream,rangeArgs,nullptr),"Depth range kernel");
        cu.check(cu.recordEvent(done,stream),"Publish AI depth completion");
    }
    void Run() noexcept {
        try{
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
            CudaThreadContext binding(cu,context);
            for(;;){
                Job j;
                {std::unique_lock<std::mutex> g(workLock);workWake.wait(g,[this]{return stopping||queued;});if(stopping)return;j=job;queued=false;}
                Infer(j);
                {std::lock_guard<std::mutex> g(workLock);submitted=true;}
            }
        }catch(...){std::lock_guard<std::mutex> g(workLock);failure=std::current_exception();}
    }
public:
    bool UsesSlot(int index)const{return active==index;}
    bool Ready(int index,uint64_t sequence)const{return opened&&slots[index].sequence==sequence&&sequence!=0;}
    CUdeviceptr Raw(int index)const{return slots[index].raw;}
    CUdeviceptr Range(int index)const{return slots[index].range;}
    ~AiDepth(){
        {std::lock_guard<std::mutex> g(workLock);stopping=true;}workWake.notify_one();if(worker.joinable())worker.join();
        if(stream)cu.syncStream(stream);
        if(graphExec)cu.graphExecDestroy(graphExec);if(fixedOutput)cu.freeMemory(fixedOutput);
        execution.reset();engine.reset();runtime.reset();
        for(auto& s:slots){if(s.raw)cu.freeMemory(s.raw);if(s.range)cu.freeMemory(s.range);}
        if(temporalRange)cu.freeMemory(temporalRange);if(input)cu.freeMemory(input);if(done)cu.destroyEvent(done);if(stream)cu.destroyStream(stream);
        if(dll)FreeLibrary(dll);
    }
};
