#pragma once
#include <NvInferRuntime.h>
#include <filesystem>
#include <fstream>
#include <memory>
// TensorRT runtime is loaded from a user-installed NVIDIA SDK; no runtime
// binary or weights are redistributed. All execution uses the capture context.
class AiDepth {
    struct Logger final:nvinfer1::ILogger {
        void log(Severity,const char*) noexcept override {}
    } logger;
    const CudaDriver& cu;
    HMODULE dll{};
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> execution;
    CUstream stream{};
    CUevent done{};
    CUdeviceptr input{};
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
    explicit AiDepth(const CudaDriver& driver):cu(driver){}
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
        if(!n||n>=32768)throw Error("Launch using Start-AI-FG.ps1 to select the TensorRT runtime");
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
        for(auto& s:slots){
            cu.check(cu.allocate(&s.raw,uint64_t(W)*H*sizeof(float)),"Allocate depth tensor");
            cu.check(cu.allocate(&s.range,2*sizeof(float)),"Allocate depth calibration");
        }
        cu.check(cu.getFunction(&prep,module,"prepareDepthRGB"),"Load depth preprocessing");
        cu.check(cu.getFunction(&rangeKernel,module,"depthRange"),"Load depth calibration");
        opened=true;
    }
    void Poll(){
        if(active>=0&&cu.complete(done)){slots[active].sequence=pending;active=-1;}
    }
    // At most one in-flight inference. No queued backlog, no CPU wait for output.
    // enqueueV3 is called on the render thread and GPU execution is asynchronous;
    // first use/deserialization can block, and TensorRT may incur host overhead.
    void Submit(int index,const CaptureFrame& frame){
        Poll();
        if(active>=0||slots[index].sequence==frame.sequence)return;
        auto& s=slots[index];s.sequence=0;
        cu.check(cu.waitEvent(stream,frame.captured,0),"AI wait for capture");
        CUdeviceptr pixels=frame.pixels;
        UINT sw=frame.width,sh=frame.height,pitch=frame.pitchPixels;
        void* args[]={&pixels,&pitch,&sw,&sh,&input};
        cu.launch(prep,stream,W,H,args);
        if(!execution->setTensorAddress("image",reinterpret_cast<void*>(input))||
           !execution->setTensorAddress("depth",reinterpret_cast<void*>(s.raw))||
           !execution->enqueueV3(reinterpret_cast<cudaStream_t>(stream)))
            throw Error("TensorRT depth enqueue failed");
        void* rangeArgs[]={&s.raw,&s.range};
        cu.check(cu.launchKernel(rangeKernel,1,1,1,1,1,1,0,stream,rangeArgs,nullptr),"Depth range kernel");
        cu.check(cu.recordEvent(done,stream),"Publish AI depth completion");
        active=index;pending=frame.sequence;
    }
    bool UsesSlot(int index)const{return active==index;}
    bool Ready(int index,uint64_t sequence)const{return opened&&slots[index].sequence==sequence&&sequence!=0;}
    CUdeviceptr Raw(int index)const{return slots[index].raw;}
    CUdeviceptr Range(int index)const{return slots[index].range;}
    ~AiDepth(){
        if(stream)cu.syncStream(stream);
        execution.reset();engine.reset();runtime.reset();
        for(auto& s:slots){if(s.raw)cu.freeMemory(s.raw);if(s.range)cu.freeMemory(s.range);}
        if(input)cu.freeMemory(input);if(done)cu.destroyEvent(done);if(stream)cu.destroyStream(stream);
        if(dll)FreeLibrary(dll);
    }
};
