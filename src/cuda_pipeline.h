#pragma once

static_assert(sizeof(void*)==8 && sizeof(NV_OF_INIT_PARAMS)==64);
static_assert(sizeof(NV_OF_EXECUTE_OUTPUT_PARAMS)==48 && sizeof(NV_OF_CUDA_API_FUNCTION_LIST)==12*sizeof(void*));

struct FlowBuffer {
    NvOFGPUBufferHandle handle{};
    CUdeviceptr pixels{};
    UINT pitchElements{};
};

class CudaPipeline {
    struct FrameState {
        FlowBuffer input, vectors;
        CUdeviceptr sceneCut{};
        CUevent ready{},lastDisplayRead{};
        CUtexObject texture{},flowTexture{};
        UINT textureWidth{},textureHeight{},texturePitch{};
        uint64_t stagedSequence{}; // owned by the optical-flow worker
    };
    struct OutputBuffer {
        ComPtr<ID3D11Buffer> pixels;
        ComPtr<ID3D11ShaderResourceView> view;
        CUgraphicsResource interop{};
        bool mapped{};
    };
    const CudaDriver& cu;
    const CaptureCuda& capture;
    SystemLibrary flowLibrary{L"nvofapi64.dll"};
    NV_OF_CUDA_API_FUNCTION_LIST api{};
    NvOFHandle flow{};
    uint64_t lastFlowSequence{};
    CUstream displayStream{},flowInputStream{},flowOutputStream{};
    CUmodule module{};
    CUfunction stageKernel{},sceneKernel{},renderKernel{};
    std::array<FrameState,kNumCapture> states{};
    std::array<OutputBuffer,2> outputs{};
    unsigned outputIndex{};

    void allocateFlowBuffer(FlowBuffer& target, UINT width, UINT height, NV_OF_BUFFER_USAGE usage,
                            NV_OF_BUFFER_FORMAT format) {
        NV_OF_BUFFER_DESCRIPTOR desc{width,height,usage,format};
        NvCheck(api.nvOFCreateGPUBufferCuda(flow,&desc,NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,&target.handle),
                "Allocate NvOFA CUDA buffer");
        target.pixels=api.nvOFGPUBufferGetCUdeviceptr(target.handle);
        NV_OF_CUDA_BUFFER_STRIDE_INFO stride{};
        NvCheck(api.nvOFGPUBufferGetStrideInfo(target.handle,&stride),"Get NvOFA CUDA pitch");
        // Input pitch counts bytes of luma. Output pitch counts signed S10.5 vector pairs.
        const UINT elementSize=(format==NV_OF_BUFFER_FORMAT_GRAYSCALE8)?1u:4u;
        UINT pitch=stride.strideInfo[0].strideXInBytes;
        if (!target.pixels || stride.numPlanes!=1 || pitch%elementSize || pitch/elementSize<width)
            throw Error("Invalid CUDA NvOFA buffer pitch");
        target.pitchElements=pitch/elementSize;
    }
    CUtexObject makeTexture(CUdeviceptr pixels,UINT width,UINT height,size_t pitch,
                            CUarray_format format,UINT channels) const {
        CUDA_RESOURCE_DESC resource{};resource.resType=CU_RESOURCE_TYPE_PITCH2D;
        resource.res.pitch2D.devPtr=pixels;resource.res.pitch2D.format=format;
        resource.res.pitch2D.numChannels=channels;resource.res.pitch2D.width=width;
        resource.res.pitch2D.height=height;resource.res.pitch2D.pitchInBytes=pitch;
        CUDA_TEXTURE_DESC desc{};desc.addressMode[0]=desc.addressMode[1]=CU_TR_ADDRESS_MODE_CLAMP;
        desc.filterMode=CU_TR_FILTER_MODE_LINEAR;desc.flags=CU_TRSF_NORMALIZED_COORDINATES;
        CUtexObject tex{};
        return cu.createTexture(&tex,&resource,&desc,nullptr)==CUDA_SUCCESS?tex:0;
    }
    void initializeFlow(UINT width, UINT height) {
        auto create=flowLibrary.symbol<decltype(&NvOFAPICreateInstanceCuda)>("NvOFAPICreateInstanceCuda");
        NvCheck(create(NV_OF_API_VERSION,&api),"Load NvOFA CUDA API");
        if (!api.nvCreateOpticalFlowCuda || !api.nvOFSetIOCudaStreams || !api.nvOFInit ||
            !api.nvOFCreateGPUBufferCuda || !api.nvOFGPUBufferGetCUdeviceptr ||
            !api.nvOFGPUBufferGetStrideInfo || !api.nvOFExecute ||
            !api.nvOFDestroyGPUBufferCuda || !api.nvOFDestroy)
            throw Error("Incomplete NvOFA CUDA API");
        NvCheck(api.nvCreateOpticalFlowCuda(capture.context,&flow),"Create CUDA NvOFA session");
        NvCheck(api.nvOFSetIOCudaStreams(flow,flowInputStream,flowOutputStream),"Set NvOFA CUDA streams");
        NV_OF_INIT_PARAMS init{};
        init.width=width; init.height=height;
        init.mode=NV_OF_MODE_OPTICALFLOW;
        init.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        init.perfLevel=NV_OF_PERF_LEVEL_FAST;
        init.enableOutputCost=NV_OF_FALSE;
        init.inputBufferFormat=NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        NvCheck(api.nvOFInit(flow,&init),"Initialize grayscale CUDA NvOFA 4x4 FAST");
        for (auto& state: states) {
            allocateFlowBuffer(state.input,width,height,NV_OF_BUFFER_USAGE_INPUT,NV_OF_BUFFER_FORMAT_GRAYSCALE8);
            allocateFlowBuffer(state.vectors,(width+kOpticalFlowGrid-1)/kOpticalFlowGrid,
                               (height+kOpticalFlowGrid-1)/kOpticalFlowGrid,
                               NV_OF_BUFFER_USAGE_OUTPUT,NV_OF_BUFFER_FORMAT_SHORT2);
            state.flowTexture=makeTexture(state.vectors.pixels,(width+3)/4,(height+3)/4,
                                          size_t(state.vectors.pitchElements)*4,CU_AD_FORMAT_SIGNED_INT16,2);
            cu.check(cu.allocate(&state.sceneCut,sizeof(unsigned)),"Allocate CUDA scene flag");
        }
    }
    void stage(int index) {
        auto& state=states[index];
        const auto& frame=capture.frames[index];
        if (state.stagedSequence==frame.sequence) return;
        // NvFBC writes at most its advertised maximum to its own slot. NvOFA
        // owns its pitched input; never pass that smaller allocation to NvFBC.
        cu.check(cu.waitEvent(flowInputStream,frame.captured,0),"Wait for NvFBC input to NvOFA");
        auto pixels=frame.pixels;
        UINT pitch=frame.pitchPixels, width=frame.width, height=frame.height;
        void* args[]={&pixels,&pitch,&state.input.pixels,&state.input.pitchElements,&width,&height};
        cu.launch(stageKernel,flowInputStream,width,height,args);
        state.stagedSequence=frame.sequence;
    }
    SourcePair pair(int current,int previous,bool useVectors) const {
        const auto& cur=capture.frames[current];
        const auto& prev=capture.frames[previous];
        SourcePair images{cur.pixels,prev.pixels,0,cur.width,cur.height,cur.pitchPixels,prev.pitchPixels,0};
        images.currentTexture=states[current].texture;
        images.previousTexture=states[previous].texture;
        // Real draws can run concurrently with lazy NvOFA allocation on the worker.
        if (useVectors) {
            images.flow=states[current].vectors.pixels;
            images.flowPitch=states[current].vectors.pitchElements;
            images.flowTexture=states[current].flowTexture;
        }
        return images;
    }
    void freeOutputs() noexcept {
        for (auto& out:outputs) {
            if (out.mapped) cu.unmap(1,&out.interop,displayStream);
            if (out.interop) cu.unregisterD3D(out.interop);
            out={};
        }
    }
public:
    CudaPipeline(const CudaDriver& driver,const CaptureCuda& source) : cu(driver),capture(source) {}
    void Init(DxDevices& dx) {
        int leastPriority=0,highestPriority=0;
        cu.check(cu.streamPriorities(&leastPriority,&highestPriority),"Get CUDA stream priority range");
        cu.check(cu.createPriorityStream(&displayStream,CU_STREAM_NON_BLOCKING,highestPriority),"Create CUDA display stream");
        cu.check(cu.createStream(&flowInputStream,CU_STREAM_NON_BLOCKING),"Create NvOFA input stream");
        cu.check(cu.createStream(&flowOutputStream,CU_STREAM_NON_BLOCKING),"Create NvOFA output stream");
        for (auto& state:states) {
            state.ready=cu.event();
            state.lastDisplayRead=cu.event();
        }
        HMODULE exe=GetModuleHandleW(nullptr);
        HRSRC resource=FindResourceW(exe,MAKEINTRESOURCEW(102),RT_RCDATA);
        if (!resource) throw Error("Embedded CUDA kernels missing",GetLastError());
        HGLOBAL data=LoadResource(exe,resource);
        const auto* bytes=static_cast<const char*>(LockResource(data));
        DWORD size=SizeofResource(exe,resource);
        if (!bytes || !size) throw Error("Read embedded CUDA kernels",GetLastError());
        std::string ptx(bytes,size);
        // Optional architecture-specific binary, with generic PTX fallback.
        HRSRC native=FindResourceW(exe,MAKEINTRESOURCEW(103),RT_RCDATA);
        if(native){
            HGLOBAL bin=LoadResource(exe,native);
            const void* image=bin?LockResource(bin):nullptr;
            if(image && cu.loadModule(&module,image)!=CUDA_SUCCESS)module=nullptr;
        }
        if(!module)cu.check(cu.loadModule(&module,ptx.c_str()),"Load embedded CUDA PTX");
        cu.check(cu.getFunction(&stageKernel,module,"stageFlowInput"),"Load NvOFA input kernel");
        cu.check(cu.getFunction(&sceneKernel,module,"sceneGate"),"Load CUDA scene-cut kernel");
        cu.check(cu.getFunction(&renderKernel,module,"renderFrame"),"Load V15 CUDA prediction kernel");
        CreateOutputs(dx);
    }
    void CreateOutputs(DxDevices& dx) {
        const uint64_t elements=uint64_t(dx.screenW)*dx.screenH;
        if (!elements || elements>UINT_MAX/4) throw Error("Invalid CUDA output dimensions");
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth=UINT(elements*4);
        desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
        view.Buffer.NumElements=UINT(elements);
        for (auto& out:outputs) {
            Check(dx.dev11->CreateBuffer(&desc,nullptr,&out.pixels),"Create CUDA/DXGI output buffer");
            Check(dx.dev11->CreateShaderResourceView(out.pixels.Get(),&view,&out.view),"Create DXGI image view");
            cu.check(cu.registerD3D(&out.interop,out.pixels.Get(),CU_GRAPHICS_REGISTER_FLAGS_NONE),
                     "Register D3D11 output for CUDA");
        }
        outputIndex=0;
    }
    void ReleaseOutputs() {
        cu.check(cu.syncStream(displayStream),"Finish CUDA output before resize");
        freeOutputs();
    }
    void NewFrame(int index) {
        const auto& frame=capture.frames[index];
        auto& state=states[index];
        // A slot is recycled only after display and optical flow readers retire.
        if(state.textureWidth!=frame.width || state.textureHeight!=frame.height || state.texturePitch!=frame.pitchPixels){
            if(state.texture)cu.check(cu.destroyTexture(state.texture),"Release resized capture texture");
            state.texture=0; state.textureWidth=frame.width; state.textureHeight=frame.height; state.texturePitch=frame.pitchPixels;
            // If the NvFBC pitch is incompatible, retain software sampling.
            state.texture=makeTexture(frame.pixels,frame.width,frame.height,size_t(frame.pitchPixels)*4,
                                      CU_AD_FORMAT_UNSIGNED_INT8,4);
        }
        cu.check(cu.waitEvent(displayStream,frame.captured,0),"Wait for captured real frame");
    }
    void Retire(int index) {
        auto& state=states[index];
        cu.check(cu.recordEvent(state.lastDisplayRead,displayStream),"Fence displayed capture");
    }
    bool Retired(int index) const {
        const auto& state=states[index];
        return cu.complete(state.lastDisplayRead);
    }
    void Calculate(int current, int previous) {
        const auto& frame=capture.frames[current];
        if (!flow) initializeFlow(frame.width,frame.height);
        stage(previous);
        stage(current);
        auto& state=states[current];
        NV_OF_EXECUTE_INPUT_PARAMS in{};
        // The V15 sampler expects backwards lookup from CURRENT into PREVIOUS.
        in.inputFrame=state.input.handle;
        in.referenceFrame=states[previous].input.handle;
        // A busy worker can skip a pair. Reuse the driver's motion history
        // only when the preceding job ended on this pair's reference frame.
        in.disableTemporalHints=lastFlowSequence==capture.frames[previous].sequence ? NV_OF_FALSE : NV_OF_TRUE;
        NV_OF_EXECUTE_OUTPUT_PARAMS out{};
        out.outputBuffer=state.vectors.handle;
        NvCheck(api.nvOFExecute(flow,&in,&out),"Execute CUDA NvOFA");
        lastFlowSequence=frame.sequence;
        SourcePair images=pair(current,previous,true);
        void* args[]={&images,&state.sceneCut};
        cu.check(cu.launchKernel(sceneKernel,1,1,1,1,1,1,0,flowOutputStream,args,nullptr),"Evaluate V15 scene cut");
        cu.check(cu.recordEvent(state.ready,flowOutputStream),"Publish optical flow for this frame pair");
    }
    bool PairReady(int current) const { return cu.complete(states[current].ready); }
    void Draw(DxDevices& dx,int current,int previous,float horizon,float radius,float feather) {
        auto& out=outputs[outputIndex];
        auto& state=states[current];
        SourcePair images=pair(current,previous,horizon>0);
        CUdeviceptr cut=horizon>0?state.sceneCut:0;
        if (horizon>0)
            cu.check(cu.waitEvent(displayStream,state.ready,0),"Order prediction after optical flow");
        cu.check(cu.map(1,&out.interop,displayStream),"Map DXGI output for CUDA");
        out.mapped=true;
        CUdeviceptr pixels{};
        size_t bytes=0;
        cu.check(cu.mappedPointer(&pixels,&bytes,out.interop),"Get CUDA output pointer");
        if (!pixels || bytes<uint64_t(dx.screenW)*dx.screenH*4)
            throw Error("Mapped CUDA output is too small");
        OutputImage target{pixels,dx.screenW,dx.screenH};
        void* args[]={&images,&target,&horizon,&radius,&feather,&cut};
        cu.launch(renderKernel,displayStream,target.width,target.height,args);
        // Unmap orders the next graphics use after the CUDA write. Present
        // needs no CPU readback, intermediate texture copy, or cuCtxSynchronize.
        cu.check(cu.unmap(1,&out.interop,displayStream),"Hand CUDA output to DXGI");
        out.mapped=false;
        dx.Present(out.view.Get());
        outputIndex=(outputIndex+1)%outputs.size();
    }
    ~CudaPipeline() {
        if (flowInputStream) cu.syncStream(flowInputStream);
        if (flowOutputStream) cu.syncStream(flowOutputStream);
        if (displayStream) cu.syncStream(displayStream);
        freeOutputs();
        for (auto& state:states) {
            if(state.texture)cu.destroyTexture(state.texture);
            if(state.flowTexture)cu.destroyTexture(state.flowTexture);
            if (state.input.handle) api.nvOFDestroyGPUBufferCuda(state.input.handle);
            if (state.vectors.handle) api.nvOFDestroyGPUBufferCuda(state.vectors.handle);
            if (state.sceneCut) cu.freeMemory(state.sceneCut);
            if (state.ready) cu.destroyEvent(state.ready);
            if (state.lastDisplayRead) cu.destroyEvent(state.lastDisplayRead);
        }
        if (flow) api.nvOFDestroy(flow);
        if (module) cu.unloadModule(module);
        if (flowInputStream) cu.destroyStream(flowInputStream);
        if (flowOutputStream) cu.destroyStream(flowOutputStream);
        if (displayStream) cu.destroyStream(displayStream);
    }
};
