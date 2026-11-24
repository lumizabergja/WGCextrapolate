#pragma once
// D3D11 acquisition only. D3D12 resources, OFA, graphics and present.
// All cross-engine dependencies are explicit. No CPU pixel readback.
struct DxDevices {
    static constexpr UINT N=8;
    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D12Device> dev12;
    ComPtr<ID3D12CommandQueue> queue,copyQueue;
    ComPtr<ID3D12CommandAllocator> copyAlloc;
    ComPtr<ID3D12GraphicsCommandList> copyList;
    HANDLE copyEvent{};
    struct SharedSource {ComPtr<ID3D11Texture2D> src; ComPtr<ID3D12Resource> dst;};
    std::vector<SharedSource> sourceCache;
    bool directSupported=true;
    std::atomic<UINT64> directFrames{0},fallbackFrames{0};
    UINT64 motionRead{};
    ComPtr<ID3D12Fence> captureFence,renderFence,ofFence;
    ComPtr<ID3D11Fence> capture11;
    UINT64 captureValue{},renderValue{},ofValue{};
    UINT64 captured[kNumCapture]{},renderRead[kNumCapture]{},ofRead[kNumCapture]{};
    ComPtr<ID3D11Texture2D> history11[kNumCapture];
    ComPtr<ID3D12Resource> history[kNumCapture],motionTexture,back[2];
    ComPtr<ID3D12CommandAllocator> alloc[N];
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 allocUsed[N]{};
    UINT ring{};
    ComPtr<ID3D12DescriptorHeap> srvHeap,rtvHeap;
    UINT srvStep{},rtvStep{};
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<IDXGISwapChain3> swap;
    HANDLE frameReady{},eventHandle{};
    bool allowTearing{};
    UINT srcW{},srcH{},screenW{},screenH{};
    ComPtr<IDXGIAdapter> matchingAdapter;
    ComPtr<IDXGIOutputDuplication> duplication;
    DXGI_OUTPUT_DESC desktop{};
    static bool Done(ID3D12Fence* f,UINT64 v){return !v||(f&&f->GetCompletedValue()>=v&&f->GetCompletedValue()!=UINT64_MAX);}
    void WaitFence(ID3D12Fence* f,UINT64 v){
        if(Done(f,v))return;
        if(f->GetCompletedValue()==UINT64_MAX)throw Error("D3D12 device removed");
        Check(f->SetEventOnCompletion(v,eventHandle),"GPU fence event");
        if(WaitForSingleObject(eventHandle,2000)!=WAIT_OBJECT_0)throw Error("GPU fence timeout");
    }
    void Drain(){
        if(captureFence&&captureValue)WaitFence(captureFence.Get(),captureValue);
        if(ofFence&&ofValue)WaitFence(ofFence.Get(),ofValue);
        if(renderFence&&renderValue)WaitFence(renderFence.Get(),renderValue);
    }
    ~DxDevices(){try{Drain();}catch(...){}if(frameReady)CloseHandle(frameReady);if(eventHandle)CloseHandle(eventHandle);if(copyEvent)CloseHandle(copyEvent);}
    void MakeTexture(UINT w,UINT h,DXGI_FORMAT format,bool share,ID3D12Resource** res){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=1;hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;
        d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if(share)d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        Check(dev12->CreateCommittedResource(&hp,share?D3D12_HEAP_FLAG_SHARED:D3D12_HEAP_FLAG_NONE,
           &d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(res)),"Create D3D12 texture");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap,UINT step,UINT i){
        auto h=heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(step)*i;return h;
    }
    void Init(HWND output,UINT w,UINT h,UINT outputW,UINT outputH){
        srcW=w;srcH=h;screenW=outputW;screenH=outputH;
        HMONITOR monitor=MonitorFromWindow(output,MONITOR_DEFAULTTONEAREST);
        ComPtr<IDXGIFactory1> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create DXGI factory");
        ComPtr<IDXGIOutput5> captureOutput;
        for(UINT i=0;!captureOutput;++i){
            ComPtr<IDXGIAdapter1> adapter;HRESULT hr=factory->EnumAdapters1(i,&adapter);
            if(hr==DXGI_ERROR_NOT_FOUND)break;Check(hr,"Enumerate adapters");
            for(UINT j=0;;++j){
                ComPtr<IDXGIOutput> candidate;hr=adapter->EnumOutputs(j,&candidate);
                if(hr==DXGI_ERROR_NOT_FOUND)break;Check(hr,"Enumerate outputs");
                Check(candidate->GetDesc(&desktop),"Read output description");
                if(desktop.Monitor!=monitor)continue;
                Check(candidate.As(&captureOutput),"Query DuplicateOutput1");
                matchingAdapter=adapter;break;
            }
        }
        if(!captureOutput||desktop.Rotation!=DXGI_MODE_ROTATION_IDENTITY)throw Error("Select an unrotated monitor");
        D3D_FEATURE_LEVEL obtained{};
        Check(D3D11CreateDevice(matchingAdapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&dev11,&obtained,&ctx11),"D3D11 capture device");
        Check(dev11.As(&dev1),"D3D11 shared resource interface");
        Check(dev11.As(&dev5),"D3D11 shared fence interface");
        Check(ctx11.As(&ctx4),"D3D11 GPU signal interface");
        const DXGI_FORMAT formats[]={DXGI_FORMAT_B8G8R8A8_UNORM};
        CheckDuplication(captureOutput->DuplicateOutput1(dev11.Get(),0,1,formats,&duplication),"DuplicateOutput1 SDR BGRA");
        Check(D3D12CreateDevice(matchingAdapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev12)),"D3D12 device");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(dev12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"D3D12 direct queue");
        q.Type=D3D12_COMMAND_LIST_TYPE_COPY;
        Check(dev12->CreateCommandQueue(&q,IID_PPV_ARGS(&copyQueue)),"D3D12 capture copy queue");
        Check(dev12->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&copyAlloc)),"Capture copy allocator");
        Check(dev12->CreateCommandList(0,q.Type,copyAlloc.Get(),nullptr,IID_PPV_ARGS(&copyList)),"Capture copy list");
        Check(copyList->Close(),"Close initial capture list");
        copyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!copyEvent)throw Error("Capture fence event",GetLastError());
        eventHandle=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!eventHandle)throw Error("Create fence event",GetLastError());
        Check(dev12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&captureFence)),"Shared capture fence");
        HANDLE shared{};
        Check(dev12->CreateSharedHandle(captureFence.Get(),nullptr,GENERIC_ALL,nullptr,&shared),"Share capture fence");
        HRESULT hr=dev5->OpenSharedFence(shared,IID_PPV_ARGS(&capture11));CloseHandle(shared);Check(hr,"Open capture fence D3D11");
        Check(dev12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&renderFence)),"Render fence");
        Check(dev12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ofFence)),"Optical flow fence");
        for(UINT i=0;i<kNumCapture;++i){
            MakeTexture(w,h,DXGI_FORMAT_B8G8R8A8_UNORM,true,history[i].GetAddressOf());
            shared=nullptr;Check(dev12->CreateSharedHandle(history[i].Get(),nullptr,GENERIC_ALL,nullptr,&shared),"Share D3D12 capture texture");
            hr=dev1->OpenSharedResource1(shared,IID_PPV_ARGS(&history11[i]));CloseHandle(shared);Check(hr,"Open D3D12 capture texture in D3D11");
        }
        MakeTexture((w+3)/4,(h+3)/4,DXGI_FORMAT_R16G16_SINT,false,motionTexture.GetAddressOf());
        ComPtr<IDXGIFactory2> f2;Check(matchingAdapter->GetParent(IID_PPV_ARGS(&f2)),"DXGI factory2");
        ComPtr<IDXGIFactory5> f5;
        if(SUCCEEDED(f2.As(&f5))){BOOL supported=FALSE;
            if(SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof(supported))))allowTearing=supported!=FALSE;}
        DXGI_SWAP_CHAIN_DESC1 sc{};sc.Width=screenW;sc.Height=screenH;sc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sc.BufferCount=2;
        sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sc.Scaling=DXGI_SCALING_STRETCH;sc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        sc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT|(allowTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0u);
        ComPtr<IDXGISwapChain1> s1;Check(f2->CreateSwapChainForHwnd(queue.Get(),output,&sc,nullptr,nullptr,&s1),"Create D3D12 flip swapchain");
        Check(s1.As(&swap),"Query swapchain3");Check(swap->SetMaximumFrameLatency(1),"Max frame latency 1");
        frameReady=swap->GetFrameLatencyWaitableObject();if(!frameReady)throw Error("Frame latency handle missing");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
        Check(dev12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvHeap)),"RTV heap");
        rtvStep=dev12->GetDescriptorHandleIncrementSize(hd.Type);
        for(UINT i=0;i<2;++i){Check(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])),"Swapchain buffer");dev12->CreateRenderTargetView(back[i].Get(),nullptr,Cpu(rtvHeap.Get(),rtvStep,i));}
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=N*3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(dev12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srvHeap)),"SRV heap");
        srvStep=dev12->GetDescriptorHandleIncrementSize(hd.Type);
        for(auto& a:alloc)Check(dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"Command allocator");
        Check(dev12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc[0].Get(),nullptr,IID_PPV_ARGS(&list)),"Graphics list");
        Check(list->Close(),"Initial list close");
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=3;
        D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges=1;rp[0].DescriptorTable.pDescriptorRanges=&range;rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[1].Constants.Num32BitValues=12;rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC samp{};samp.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU=samp.AddressV=samp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;samp.MaxAnisotropy=1;
        samp.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;samp.MaxLOD=D3D12_FLOAT32_MAX;samp.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=rp;rs.NumStaticSamplers=1;rs.pStaticSamplers=&samp;
        rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob,errs;Check(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errs),"Serialize root signature");
        Check(dev12->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Root signature");
        static const char* shader=R"HLSL(
Texture2D<float4> currentFrame:register(t0);
Texture2D<int2> rawFlow:register(t1);
Texture2D<float4> previousFrame:register(t2);
SamplerState samp:register(s0);
cbuffer Settings:register(b0){float horizon;float2 motionInv;float maxMotion;float2 gridExtent;float gridSize;float _padding1;float centerRadiusPx;float centerFeatherPx;float2 outputSize;};
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;nointerpolation float scene:TEXCOORD1;};
// Use native block centers, including partial cells on odd-sized captures.
float2 flow(float2 uv){
    uint fw,fh;rawFlow.GetDimensions(fw,fh);
    float2 pos=uv/(motionInv*gridSize*gridExtent)*float2(fw,fh)-0.5;
    int2 base=int2(floor(pos));float2 f=frac(pos);
    int2 edge=int2(int(fw)-1,int(fh)-1);
    int2 a=clamp(base,int2(0,0),edge),b=clamp(base+int2(1,0),int2(0,0),edge);
    int2 c=clamp(base+int2(0,1),int2(0,0),edge),d=clamp(base+int2(1,1),int2(0,0),edge);
    float2 v00=float2(rawFlow.Load(int3(a,0)))*(1.0/32.0);
    float2 v10=float2(rawFlow.Load(int3(b,0)))*(1.0/32.0);
    float2 v01=float2(rawFlow.Load(int3(c,0)))*(1.0/32.0);
    float2 v11=float2(rawFlow.Load(int3(d,0)))*(1.0/32.0);
    return lerp(lerp(v00,v10,f.x),lerp(v01,v11,f.x),f.y)*motionInv;
}
float cutSample(float2 uv){float2 q=uv+flow(uv);if(any(q<0)||any(q>1))return 0;return dot(abs(currentFrame.SampleLevel(samp,uv,0).rgb-previousFrame.SampleLevel(samp,q,0).rgb),float3(.333333,.333333,.333333));}
V VS(uint id:SV_VertexID){
    V o;float2 p=float2(id==2?3.0:-1.0,id==1?3.0:-1.0);
    o.p=float4(p,0,1);o.uv=float2((p.x+1)*0.5,1-(p.y+1)*0.5);o.scene=0;
    [branch] if(horizon>0.00001){float e=cutSample(float2(.2,.3))+cutSample(float2(.8,.3))+cutSample(float2(.5,.5))+cutSample(float2(.2,.7))+cutSample(float2(.8,.7));o.scene=smoothstep(.19,.32,e*.2);}
    return o;
}
float centerMask(float2 uv){
    // The center mask affects only predicted pixels, never real captured frames.
    // 0px radius + 0px feather is a full bypass (no center protection).
    if(centerRadiusPx<=0.0 && centerFeatherPx<=0.0)return 1.0;
    float distancePx=length((uv-float2(.5,.5))*outputSize);
    if(centerFeatherPx<=0.0)return distancePx>centerRadiusPx?1.0:0.0;
    return smoothstep(centerRadiusPx,centerRadiusPx+centerFeatherPx,distancePx);
}
float4 PS(V i):SV_Target {
    // NVIDIA's stretched scanout is already scaled into native game-size textures.
    float2 uv=i.uv;
    float4 base=currentFrame.SampleLevel(samp,uv,0);
    [branch] if(horizon<=0.00001 || i.scene>0.9)return base;
    float2 v=flow(uv),q=uv+horizon*v;
    if(any(q<0)||any(q>1))return base;
    float2 mv=flow(q),old=q+mv;
    if(any(old<0)||any(old>1)||length(mv)>maxMotion)return base;
    float4 next=currentFrame.SampleLevel(samp,q,0);
    float3 prev=previousFrame.SampleLevel(samp,old,0).rgb;
    float residual=dot(abs(next.rgb-prev),float3(.333333,.333333,.333333));
    float confidence=(1-smoothstep(.045,.20,residual))*(1-smoothstep(.003,.025,length(mv-v)));
    return lerp(base,next,saturate(confidence*centerMask(uv)));
}
)HLSL";

        ComPtr<ID3DBlob> vs,ps;errs.Reset();
        Check(D3DCompile(shader,strlen(shader),"FG",nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errs),"Compile VS");
        errs.Reset();Check(D3DCompile(shader,strlen(shader),"FG",nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&errs),"Compile PS");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();
        pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
        auto& blend=pd.BlendState.RenderTarget[0];blend.SrcBlend=D3D12_BLEND_ONE;blend.DestBlend=D3D12_BLEND_ZERO;blend.BlendOp=D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable=FALSE;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.StencilReadMask=D3D12_DEFAULT_STENCIL_READ_MASK;pd.DepthStencilState.StencilWriteMask=D3D12_DEFAULT_STENCIL_WRITE_MASK;
        pd.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
        pd.DepthStencilState.BackFace=pd.DepthStencilState.FrontFace;
        pd.SampleMask=UINT_MAX;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets=1;pd.RTVFormats[0]=sc.Format;pd.SampleDesc.Count=1;
        Check(dev12->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"Create D3D12 extrapolation PSO");
    }
    // Looking Glass-style: try opening the acquired duplication texture as a D3D12 resource.
    // Do not keep reading an acquired image after ReleaseFrame. Copy into private history first.
    ID3D12Resource* OpenSource(ID3D11Texture2D* tex){
        if(!directSupported)return nullptr;
        for(auto& entry:sourceCache)if(entry.src.Get()==tex)return entry.dst.Get();
        if(sourceCache.size()>=16){directSupported=false;return nullptr;}
        ComPtr<IDXGIResource1> res;
        if(FAILED(tex->QueryInterface(IID_PPV_ARGS(&res)))){directSupported=false;return nullptr;}
        HANDLE handle{};
        if(FAILED(res->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ,nullptr,&handle))){directSupported=false;return nullptr;}
        SharedSource entry;entry.src=tex;
        HRESULT hr=dev12->OpenSharedHandle(handle,IID_PPV_ARGS(&entry.dst));CloseHandle(handle);
        if(FAILED(hr)){directSupported=false;return nullptr;}
        sourceCache.push_back(std::move(entry));return sourceCache.back().dst.Get();
    }
    void CompleteCapture(UINT64 v){
        if(Done(captureFence.Get(),v))return;
        if(captureFence->GetCompletedValue()==UINT64_MAX)throw Error("Capture device removed");
        Check(captureFence->SetEventOnCompletion(v,copyEvent),"Capture fence event");
        if(WaitForSingleObject(copyEvent,2000)!=WAIT_OBJECT_0)throw Error("Capture GPU fence timeout");
    }
    bool Grab(HWND game,int slot,UINT timeout,LARGE_INTEGER& timestamp){
        // Slot ownership is reserved by the mailbox producer before acquisition.
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr=duplication->AcquireNextFrame(timeout,&info,&resource);
        if(hr==DXGI_ERROR_WAIT_TIMEOUT)return false;
        CheckDuplication(hr,"Acquire desktop frame");
        struct Lease {
            IDXGIOutputDuplication* owner;
            ~Lease(){if(owner)owner->ReleaseFrame();}
        } lease{duplication.Get()};
        // DXGI has already masked protected pixels in the acquired texture.
        bool updated=info.LastPresentTime.QuadPart!=0;
        if(updated){
            RECT client{};
            POINT origin{};
            if(!GetClientRect(game,&client) || !ClientToScreen(game,&origin))return false;
            if(UINT(client.right)!=srcW || UINT(client.bottom)!=srcH ||
               MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST)!=desktop.Monitor)throw DisplayChanged{};
            LONG x=origin.x-desktop.DesktopCoordinates.left,y=origin.y-desktop.DesktopCoordinates.top;
            if(x<0 || y<0 || x+LONG(srcW)>desktop.DesktopCoordinates.right-desktop.DesktopCoordinates.left ||
               y+LONG(srcH)>desktop.DesktopCoordinates.bottom-desktop.DesktopCoordinates.top)
                throw Error("Keep the entire game client on the selected monitor");
            ComPtr<ID3D11Texture2D> texture;
            Check(resource.As(&texture),"Read desktop texture");
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            if(UINT(x)+srcW>desc.Width || UINT(y)+srcH>desc.Height)throw DisplayChanged{};
            D3D11_BOX region{UINT(x),UINT(y),0,UINT(x)+srcW,UINT(y)+srcH,1};
            if(desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)throw Error("UltraDirect requires SDR BGRA8 capture");
            if(auto* source=OpenSource(texture.Get())){
                Check(ctx4->Signal(capture11.Get(),++captureValue),"Signal acquired image");
                ctx11->Flush();
                Check(copyQueue->Wait(captureFence.Get(),captureValue),"Copy queue wait for acquisition");
                Check(copyAlloc->Reset(),"Reset copy allocator");
                Check(copyList->Reset(copyAlloc.Get(),nullptr),"Reset capture copy list");
                D3D12_TEXTURE_COPY_LOCATION from{},to{};
                from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.pResource=history[slot].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_BOX box{region.left,region.top,region.front,region.right,region.bottom,region.back};
                copyList->CopyTextureRegion(&to,0,0,0,&from,&box);
                Check(copyList->Close(),"Close capture copy list");
                ID3D12CommandList* lists[]={copyList.Get()};copyQueue->ExecuteCommandLists(1,lists);
                Check(copyQueue->Signal(captureFence.Get(),++captureValue),"Copy complete");
                ++directFrames;
            }else{
                ctx11->CopySubresourceRegion(history11[slot].Get(),0,0,0,0,texture.Get(),0,&region);
                Check(ctx4->Signal(capture11.Get(),++captureValue),"Signal fallback capture GPU fence");
                ctx11->Flush();++fallbackFrames;
            }
            // Acquisition lifetime stays valid until the GPU has finished reading it.
            // Only this capture thread waits; the presenter never waits on this copy.
            CompleteCapture(captureValue);
            captured[slot]=captureValue;
            timestamp=info.LastPresentTime;
        }
        lease.owner=nullptr;
        CheckDuplication(duplication->ReleaseFrame(),"Release desktop frame");
        return updated;
    }

    struct Params{float horizon,invW,invH,maxMotion,flowW,flowH,grid,pad,centerRadiusPx,centerFeatherPx,outputW,outputH;};
    static_assert(sizeof(Params)==48);
    void Srv(UINT i,ID3D12Resource* resource,DXGI_FORMAT format){
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=format;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;
        dev12->CreateShaderResourceView(resource,&v,Cpu(srvHeap.Get(),srvStep,i));
    }
    void Barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=r;
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=from;b.Transition.StateAfter=to;list->ResourceBarrier(1,&b);
    }
    void Draw(UINT current,UINT previous,ID3D12Resource* rawFlow,float horizon,int grid){
        const bool predicted=horizon>0&&rawFlow;
        const UINT i=ring++%N;
        // Normally completed well before reuse; bounded wait only on severe GPU backpressure.
        WaitFence(renderFence.Get(),allocUsed[i]);
        Check(alloc[i]->Reset(),"Reset allocator");Check(list->Reset(alloc[i].Get(),pso.Get()),"Reset graphics list");
        const UINT bb=swap->GetCurrentBackBufferIndex();
        // Shared/simultaneous-access textures implicitly promote from COMMON and decay on completion.
        if(srcW==screenW&&srcH==screenH&&!predicted){
            Barrier(back[bb].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(back[bb].Get(),history[current].Get());
            Barrier(back[bb].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
        }else{
            Srv(i*3,history[current].Get(),DXGI_FORMAT_B8G8R8A8_UNORM);
            Srv(i*3+1,predicted?rawFlow:nullptr,DXGI_FORMAT_R16G16_SINT);
            Srv(i*3+2,history[previous].Get(),DXGI_FORMAT_B8G8R8A8_UNORM);
            Barrier(back[bb].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto rtv=Cpu(rtvHeap.Get(),rtvStep,bb);
            list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
            D3D12_VIEWPORT vp{0,0,float(screenW),float(screenH),0,1};
            D3D12_RECT rect{0,0,LONG(screenW),LONG(screenH)};
            list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&rect);
            list->SetGraphicsRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]={srvHeap.Get()};list->SetDescriptorHeaps(1,heaps);
            auto gpu=srvHeap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(srvStep)*i*3;
            list->SetGraphicsRootDescriptorTable(0,gpu);
            const Params p{horizon,1.f/srcW,1.f/srcH,0.12f,float((srcW+grid-1)/grid),float((srcH+grid-1)/grid),float(grid),0,
               float(g_centerRadiusPx.load()),float(g_centerFeatherPx.load()),float(screenW),float(screenH)};
            list->SetGraphicsRoot32BitConstants(1,12,&p,0);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
            Barrier(back[bb].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        }
        Check(list->Close(),"Close graphics list");
        const UINT64 cap=std::max(captured[current],captured[previous]);
        if(!Done(captureFence.Get(),cap))Check(queue->Wait(captureFence.Get(),cap),"GPU wait for capture");
        if(predicted&&!Done(ofFence.Get(),ofValue))Check(queue->Wait(ofFence.Get(),ofValue),"GPU wait for optical flow");
        ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        Check(queue->Signal(renderFence.Get(),++renderValue),"Render GPU signal");
        allocUsed[i]=renderValue;renderRead[current]=renderValue;renderRead[previous]=renderValue;
        if(predicted)motionRead=renderValue;
        Check(swap->Present(0,allowTearing?DXGI_PRESENT_ALLOW_TEARING:0),"D3D12 present");
    }
    void PresentReal(UINT current,int grid){Draw(current,current,nullptr,0,grid);}
};

struct Flow11 { // name retained only for compatibility with existing scheduler
    DxDevices* dx{};
    HMODULE library{};
    NV_OF_D3D12_API_FUNCTION_LIST funcs{};
    NvOFHandle handle{};
    NvOFGPUBufferHandle frames[kNumCapture]{},motion{};
    ID3D12Resource* motionSRV{};
    void Register(ID3D12Resource* res,NvOFGPUBufferHandle& buffer){
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p{};
        p.resource=res;p.hOFGpuBuffer=&buffer;p.inputFencePoint={dx->ofFence.Get(),dx->ofValue};
        p.outputFencePoint={dx->ofFence.Get(),dx->ofValue+1};
        NvCheck(funcs.nvOFRegisterResourceD3D12(handle,&p),"NvOFA D3D12 register");
        ++dx->ofValue;dx->WaitFence(dx->ofFence.Get(),dx->ofValue);
    }
    void Init(DxDevices& d,int grid){
        dx=&d;library=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!library)throw Error("nvofapi64.dll missing");
        auto entry=reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(GetProcAddress(library,"NvOFAPICreateInstanceD3D12"));
        if(!entry)throw Error("NvOFAPICreateInstanceD3D12 missing");
        NvCheck(entry(NV_OF_API_VERSION,&funcs),"NvOFA D3D12 API");
        if(!funcs.nvCreateOpticalFlowD3D12||!funcs.nvOFInit||!funcs.nvOFExecuteD3D12||
           !funcs.nvOFRegisterResourceD3D12||!funcs.nvOFUnregisterResourceD3D12||
           !funcs.nvOFGetSurfaceFormatCountD3D12||!funcs.nvOFGetSurfaceFormatD3D12||!funcs.nvOFDestroy)throw Error("NvOFA D3D12 function list incomplete");
        NvCheck(funcs.nvCreateOpticalFlowD3D12(d.dev12.Get(),&handle),"Create native D3D12 NvOFA");
        NV_OF_INIT_PARAMS init{};init.width=d.srcW;init.height=d.srcH;
        init.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;init.mode=NV_OF_MODE_OPTICALFLOW;init.perfLevel=NV_OF_PERF_LEVEL_FAST;
        init.enableOutputCost=NV_OF_FALSE;init.enableExternalHints=NV_OF_FALSE;init.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;
        if(grid!=4)throw Error("UltraDirect requires 4x4 flow");
        NvCheck(funcs.nvOFInit(handle,&init),"NvOFA D3D12 FAST 4x4 init");
        auto supported=[&](NV_OF_BUFFER_USAGE usage,DXGI_FORMAT format){
            UINT count=0;NvCheck(funcs.nvOFGetSurfaceFormatCountD3D12(handle,usage,NV_OF_MODE_OPTICALFLOW,&count),"NvOFA D3D12 formats count");
            if(!count||count>64)throw Error("NvOFA D3D12 formats unavailable");
            std::vector<DXGI_FORMAT> f(count);NvCheck(funcs.nvOFGetSurfaceFormatD3D12(handle,usage,NV_OF_MODE_OPTICALFLOW,f.data()),"NvOFA D3D12 formats");
            if(std::find(f.begin(),f.end(),format)==f.end())throw Error("NvOFA required D3D12 texture format unsupported");
        };
        supported(NV_OF_BUFFER_USAGE_INPUT,DXGI_FORMAT_B8G8R8A8_UNORM);
        supported(NV_OF_BUFFER_USAGE_OUTPUT,DXGI_FORMAT_R16G16_SINT);
        for(int i=0;i<kNumCapture;++i)Register(d.history[i].Get(),frames[i]);
        Register(d.motionTexture.Get(),motion);motionSRV=d.motionTexture.Get();
    }
    bool Calculate(int current,int previous){
        // Only one flow request outstanding; skip rather than enqueue stale work.
        if(!DxDevices::Done(dx->ofFence.Get(),dx->ofValue)||!DxDevices::Done(dx->renderFence.Get(),dx->motionRead))return false;
        // Wait on GPU, not CPU. Prior flow and rendering must retire before reusing output.
        NV_OF_FENCE_POINT waits[3]={{dx->captureFence.Get(),std::max(dx->captured[current],dx->captured[previous])},
            {dx->renderFence.Get(),dx->motionRead},{dx->ofFence.Get(),dx->ofValue}};
        NV_OF_FENCE_POINT done{dx->ofFence.Get(),dx->ofValue+1};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};in.inputFrame=frames[current];in.referenceFrame=frames[previous];
        in.numFencePoints=3;in.fencePoint=waits;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};out.outputBuffer=motion;out.fencePoint=&done;
        NvCheck(funcs.nvOFExecuteD3D12(handle,&in,&out),"Native D3D12 NvOFA execute");
        dx->ofValue=done.value;dx->ofRead[current]=done.value;dx->ofRead[previous]=done.value;
        return true;
    }
    ~Flow11(){
        if(dx){try{dx->Drain();}catch(...){}}
        if(handle){
            for(auto f:frames)if(f&&funcs.nvOFUnregisterResourceD3D12){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{f};funcs.nvOFUnregisterResourceD3D12(&p);}
            if(motion&&funcs.nvOFUnregisterResourceD3D12){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{motion};funcs.nvOFUnregisterResourceD3D12(&p);}
            if(funcs.nvOFDestroy)funcs.nvOFDestroy(handle);
        }
        if(library)FreeLibrary(library);
    }
};

// Single latest-frame mailbox. Producer owns DXGI and the copy queue.
// Consumer pins current/previous; a released slot may be reused only after BOTH GPU fences retire.
struct CaptureMailbox {
    DxDevices& dx;
    HWND game;
    DWORD pid;
    std::mutex lock;
    std::atomic<bool> stopping{false};
    std::thread thread;
    int latest=-1,pinCurrent=-1,pinPrevious=-1;
    LARGE_INTEGER stamp{};
    std::exception_ptr failure;
    bool active=false;
    UINT epoch=0;
    CaptureMailbox(DxDevices& d,HWND h,DWORD p):dx(d),game(h),pid(p){
        thread=std::thread([this]{Run();});
    }
    ~CaptureMailbox(){Stop();}
    void Stop(){stopping=true;if(thread.joinable())thread.join();}
    void SetActive(bool enabled){
        std::lock_guard<std::mutex> guard(lock);
        if(active==enabled)return;
        active=enabled;++epoch;latest=-1;pinCurrent=pinPrevious=-1;
    }
    int Take(LARGE_INTEGER& ts){
        std::lock_guard<std::mutex> guard(lock);
        if(failure)std::rethrow_exception(failure);
        if(latest<0)return -1;
        pinPrevious=pinCurrent;pinCurrent=latest;latest=-1;ts=stamp;return pinCurrent;
    }
    void Run() noexcept{
        try{
            while(!stopping.load(std::memory_order_acquire)){
                int slot=-1;UINT e=0;
                {
                    std::lock_guard<std::mutex> guard(lock);
                    if(active){
                        e=epoch;
                        for(int i=0;i<kNumCapture;++i){
                            if(i==pinCurrent||i==pinPrevious||i==latest)continue;
                            if(!DxDevices::Done(dx.renderFence.Get(),dx.renderRead[i])||
                               !DxDevices::Done(dx.ofFence.Get(),dx.ofRead[i]))continue;
                            slot=i;break;
                        }
                    }
                }
                if(slot<0){Sleep(1);continue;}
                DWORD fg=0;GetWindowThreadProcessId(GetForegroundWindow(),&fg);
                if(fg!=pid||IsIconic(game)){Sleep(1);continue;}
                LARGE_INTEGER ts{};
                if(!dx.Grab(game,slot,4,ts))continue;
                {
                    std::lock_guard<std::mutex> guard(lock);
                    if(active&&epoch==e){latest=slot;stamp=ts;}
                }
            }
        }catch(...){
            std::lock_guard<std::mutex> guard(lock);failure=std::current_exception();
        }
    }
};
