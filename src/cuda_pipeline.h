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
        CUevent ready{},lastDisplayRead{},lumaReady{};
        CUtexObject texture{},flowTexture{};
        UINT textureWidth{},textureHeight{},texturePitch{};
        uint64_t stagedSequence{}; // render-thread only; worker sees published events
    };
    struct OutputBuffer {
        ComPtr<ID3D11Buffer> pixels;
        ComPtr<ID3D11ShaderResourceView> view;
        CUgraphicsResource interop{};
        bool mapped{};
        ComPtr<ID3D11Buffer> depthPixels;
        ComPtr<ID3D11ShaderResourceView> depthView;
        CUgraphicsResource depthInterop{};
        bool depthMapped{};
    };
    const CudaDriver& cu;
    const CaptureCuda& capture;
    std::unique_ptr<AiDepth> ai;
    SystemLibrary flowLibrary{L"nvofapi64.dll"};
    NV_OF_CUDA_API_FUNCTION_LIST api{};
    NvOFHandle flow{};
    uint64_t lastFlowSequence{};
    CUstream displayStream{},flowInputStream{},flowOutputStream{},lumaStream{};
    CUmodule module{};
    CUfunction stageKernel{},sceneKernel{},renderKernel{},depthKernel{};
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
            // NvOFA owns this allocation. Use bounded SHORT2 loads, not a
            // texture view over an externally managed pitched resource.
            state.flowTexture=0;
            cu.check(cu.allocate(&state.sceneCut,sizeof(unsigned)),"Allocate CUDA scene flag");
        }
    }
    void stage(int index) {
        auto& state=states[index];
        const auto& frame=capture.frames[index];
        if (state.stagedSequence==frame.sequence) return;
        // NvFBC writes at most its advertised maximum to its own slot. NvOFA
        // owns its pitched input; never pass that smaller allocation to NvFBC.
        if(!frame.sequence || !frame.width || !frame.height || !state.input.pixels ||
            state.input.pitchElements<frame.width || frame.pitchPixels<frame.width)
            throw Error("Invalid luma preparation buffer");
        cu.check(cu.waitEvent(lumaStream,frame.captured,0),"Wait for NvFBC input to luma");
        auto pixels=frame.pixels;
        UINT pitch=frame.pitchPixels, width=frame.width, height=frame.height;
        void* args[]={&pixels,&pitch,&state.input.pixels,&state.input.pitchElements,&width,&height};
        cu.launch(stageKernel,lumaStream,width,height,args);
        cu.diagnosticSync(lumaStream,"Diagnostic: overlapping luma conversion");
        cu.check(cu.recordEvent(state.lumaReady,lumaStream),"Publish prepared luma");
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
            if(out.depthMapped)cu.unmap(1,&out.depthInterop,displayStream);
            if(out.depthInterop)cu.unregisterD3D(out.depthInterop);
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
        cu.check(cu.createStream(&lumaStream,CU_STREAM_NON_BLOCKING),"Create independent luma preparation stream");
        cu.check(cu.createStream(&flowOutputStream,CU_STREAM_NON_BLOCKING),"Create NvOFA output stream");
        for (auto& state:states) {
            state.ready=cu.event();
            state.lastDisplayRead=cu.event();
            state.lumaReady=cu.event();
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
        cu.check(cu.getFunction(&depthKernel,module,"renderAiDepth"),"Load depth reprojection kernel");
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
    void PollDepth(){if(ai)ai->Poll();}
    bool DepthReady(int current,int previous,bool flowReady){
        PollDepth();return ai&&(ai->Ready(current,capture.frames[current].sequence)||(flowReady&&previous>=0&&ai->Ready(previous,capture.frames[previous].sequence)));
    }
    void PrepareDepth(int current){
        if(!ai){ai=std::make_unique<AiDepth>(cu);ai->Open(module);}
        ai->Submit(current,capture.frames[current]);
    }
    void NewFrame(int index,bool prepareLuma) {
        const auto& frame=capture.frames[index];
        auto& state=states[index];
        // A slot is recycled only after display and optical flow readers retire.
        if(state.textureWidth!=frame.width || state.textureHeight!=frame.height || state.texturePitch!=frame.pitchPixels){
            if(state.texture)cu.check(cu.destroyTexture(state.texture),"Release resized capture texture");
            state.texture=0; state.textureWidth=frame.width; state.textureHeight=frame.height; state.texturePitch=frame.pitchPixels;
            // The NvFBC allocation can end at the last visible pixel rather
            // than at the end of a complete pitched texture row. Keep the
            // original bounded color reads until a texture view is validated.
            state.texture=0;
        }
        cu.check(cu.waitEvent(displayStream,frame.captured,0),"Wait for captured real frame");
        // Initialize on the render thread before the first request is published.
        // After initialization only the worker calls nvOFExecute.
        if(prepareLuma){
            if(!flow)initializeFlow(frame.width,frame.height);
            stage(index);
        }
    }
    void PreparePair(int current,int previous) {
        // Called by the render thread before Request, including after switching
        // from Off. Existing staged events are never re-recorded for the same
        // capture sequence. The mailbox keeps both slots owned.
        if(!flow)initializeFlow(capture.frames[current].width,capture.frames[current].height);
        stage(previous);
        stage(current);
    }
    void Retire(int index) {
        auto& state=states[index];
        cu.check(cu.recordEvent(state.lastDisplayRead,displayStream),"Fence displayed capture");
    }
    bool Retired(int index) {
        const auto& state=states[index];
        // A source slot cannot be overwritten until BOTH its display and
        // grayscale reads finish; the caller also checks active OFA ownership.
        PollDepth();
        return (!ai||!ai->UsesSlot(index)) && cu.complete(state.lastDisplayRead) && cu.complete(state.lumaReady);
    }
    void Calculate(int current, int previous) {
        const auto& frame=capture.frames[current];
        // Both events were recorded before the render thread published this
        // request. Use GPU dependencies rather than a CPU synchronization.
        cu.check(cu.waitEvent(flowInputStream,states[previous].lumaReady,0),"Wait for reference luma");
        cu.check(cu.waitEvent(flowInputStream,states[current].lumaReady,0),"Wait for current luma");
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
        cu.diagnosticSync(flowOutputStream,"Diagnostic: NvOFA output");
        lastFlowSequence=frame.sequence;
        SourcePair images=pair(current,previous,true);
        void* args[]={&images,&state.sceneCut};
        cu.check(cu.launchKernel(sceneKernel,1,1,1,1,1,1,0,flowOutputStream,args,nullptr),"Evaluate V15 scene cut");
        cu.diagnosticSync(flowOutputStream,"Diagnostic: scene gate");
        cu.check(cu.recordEvent(state.ready,flowOutputStream),"Publish optical flow for this frame pair");
    }
    bool PairReady(int current) const { return cu.complete(states[current].ready); }
    void Draw(DxDevices& dx,int current,int previous,float horizon,float radius,float feather,bool flowReady) {
        dx.reflex.Begin(dx.dev11.Get(),g_reflexEnabled.load(std::memory_order_relaxed));
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
        PollDepth();
        const bool depthReady=g_warpEnabled.load(std::memory_order_relaxed)&&DepthReady(current,previous,flowReady);
        if(depthReady){
            if(!out.depthInterop){
                D3D11_BUFFER_DESC d{};if(uint64_t(target.width)*target.height>UINT_MAX/16)throw Error("Guidance output too large");d.ByteWidth=target.width*target.height*16;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                Check(dx.dev11->CreateBuffer(&d,nullptr,&out.depthPixels),"AI depth output buffer");
                D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;v.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;v.Buffer.NumElements=target.width*target.height;
                Check(dx.dev11->CreateShaderResourceView(out.depthPixels.Get(),&v,&out.depthView),"AI depth output view");
                cu.check(cu.registerD3D(&out.depthInterop,out.depthPixels.Get(),CU_GRAPHICS_REGISTER_FLAGS_NONE),"Register AI depth output");
            }
            cu.check(cu.map(1,&out.depthInterop,displayStream),"Map AI depth output");out.depthMapped=true;
            CUdeviceptr d{};size_t size=0;cu.check(cu.mappedPointer(&d,&size,out.depthInterop),"Get AI depth output");
            if(!d||size<uint64_t(target.width)*target.height*16)throw Error("AI depth output too small");
            OutputImage depthTarget{d,target.width,target.height};
            unsigned previousDepth=ai->Ready(current,capture.frames[current].sequence)?0u:1u;
            int depthSlot=previousDepth?previous:current;
            CUdeviceptr raw=ai->Raw(depthSlot),range=ai->Range(depthSlot);
            SourcePair guidance=pair(current,previous,flowReady);CUdeviceptr guidanceCut=flowReady?state.sceneCut:0;
            if(flowReady)cu.check(cu.waitEvent(displayStream,state.ready,0),"Order guidance after optical flow");
            float interval=dx.MotionInterval(dx.warpSequence,dx.warpHorizon);
            void* depthArgs[]={&guidance,&depthTarget,&horizon,&radius,&feather,&guidanceCut,&raw,&range,&interval,&previousDepth};
            cu.launch(depthKernel,displayStream,target.width,target.height,depthArgs);
            cu.check(cu.unmap(1,&out.depthInterop,displayStream),"Hand AI depth to D3D11");out.depthMapped=false;
        }
        cu.diagnosticSync(displayStream,horizon>0?"Diagnostic: extrapolation kernel":"Diagnostic: real-frame kernel");
        // Unmap orders the next graphics use after the CUDA write. Present
        // needs no CPU readback, intermediate texture copy, or cuCtxSynchronize.
        cu.check(cu.unmap(1,&out.interop,displayStream),"Hand CUDA output to DXGI");
        out.mapped=false;
        dx.Present(out.view.Get(),depthReady?out.depthView.Get():nullptr);
        outputIndex=(outputIndex+1)%outputs.size();
    }
    ~CudaPipeline() {
        // AsyncFlow joins its worker before this destructor runs.
        if(lumaStream)cu.syncStream(lumaStream);
        if (flowInputStream) cu.syncStream(flowInputStream);
        if (flowOutputStream) cu.syncStream(flowOutputStream);
        if (displayStream) cu.syncStream(displayStream);
        ai.reset();freeOutputs();
        for (auto& state:states) {
            if(state.texture)cu.destroyTexture(state.texture);
            if(state.flowTexture)cu.destroyTexture(state.flowTexture);
            if (state.input.handle) api.nvOFDestroyGPUBufferCuda(state.input.handle);
            if (state.vectors.handle) api.nvOFDestroyGPUBufferCuda(state.vectors.handle);
            if (state.sceneCut) cu.freeMemory(state.sceneCut);
            if (state.lumaReady) cu.destroyEvent(state.lumaReady);
            if (state.ready) cu.destroyEvent(state.ready);
            if (state.lastDisplayRead) cu.destroyEvent(state.lastDisplayRead);
        }
        if (flow) api.nvOFDestroy(flow);
        if (module) cu.unloadModule(module);
        if(lumaStream)cu.destroyStream(lumaStream);
        if (flowInputStream) cu.destroyStream(flowInputStream);
        if (flowOutputStream) cu.destroyStream(flowOutputStream);
        if (displayStream) cu.destroyStream(displayStream);
    }
};
