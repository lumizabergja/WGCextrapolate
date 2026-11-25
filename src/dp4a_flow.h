// Included after DxDevices. All resource ownership stays on the render thread.

void CuCheck(cudaError_t e,const char* where){
    if(e!=cudaSuccess)throw std::runtime_error(std::string(where)+": "+cudaGetErrorString(e));
}
struct Flow11 {
    DxDevices* owner{};
    struct Input {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11UnorderedAccessView> uav;
        cudaGraphicsResource* resource{};
    } input[2];
    struct MotionSlot {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> srv;
        cudaGraphicsResource* resource{};
        winrt::handle completionEvent;
        UINT64 submitted{},lastRead{},completionArmed{};
        uint64_t pairSerial{};
        int current{-1},previous{-1};
    } slots[2];
    ComPtr<ID3D11Fence> flowFence;
    ComPtr<ID3D11ComputeShader> luma;
    cudaStream_t stream{};
    UINT halfW{},halfH{},packedW{},fw{},fh{};
    UINT64 fenceSerial{},lastCompute{};
    int active{-1};
    bool mapped{};
    cudaGraphicsResource* mapping[3]{};
    void Init(DxDevices& d,int grid){
        owner=&d;
        int cudaDevice=-1;
        CuCheck(cudaD3D11GetDevice(&cudaDevice,d.matchingAdapter.Get()),"Find CUDA device for DX11 adapter");
        CuCheck(cudaSetDevice(cudaDevice),"Select CUDA device");
        cudaDeviceProp props{};CuCheck(cudaGetDeviceProperties(&props,cudaDevice),"Query CUDA device");
        if(props.major*10+props.minor<120)throw Error("This build targets Blackwell SM 120 or newer");
        CuCheck(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking),"Create CUDA motion stream");
        ComPtr<ID3D11Device5> fenceDevice;
        Check(d.dev11.As(&fenceDevice),"Query DP4a fence device");
        Check(fenceDevice->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&flowFence)),"Create DP4a fence");
        halfW=(d.srcW+1)/2;halfH=(d.srcH+1)/2;packedW=(halfW+3)/4;
        fw=(d.srcW+UINT(grid)-1)/UINT(grid);fh=(d.srcH+UINT(grid)-1)/UINT(grid);
        // WGC surfaces are sampled directly. Only downsampled luma is staged for CUDA.
        const char* shader=R"HLSL(
Texture2D<float4> currentFrame:register(t0);
Texture2D<float4> previousFrame:register(t1);
RWBuffer<uint> currentLuma:register(u0);
RWBuffer<uint> previousLuma:register(u1);
uint lum(float3 rgb){return (uint)round(saturate(dot(rgb,float3(.2126,.7152,.0722)))*255.0);}
[numthreads(16,8,1)]
void CS(uint3 tid:SV_DispatchThreadID){
 uint w,h;currentFrame.GetDimensions(w,h);uint ow=((w+1)/2+3)/4,oh=(h+1)/2;
 if(tid.x>=ow||tid.y>=oh)return;
 uint a=0,b=0;
 [unroll]for(uint k=0;k<4;++k){
  uint2 p=min(uint2((tid.x*4+k)*2,tid.y*2),uint2(w-1,h-1));
  uint2 q=min(p+uint2(1,1),uint2(w-1,h-1));
  float3 c=(currentFrame.Load(int3(p,0)).rgb+currentFrame.Load(int3(q.x,p.y,0)).rgb+currentFrame.Load(int3(p.x,q.y,0)).rgb+currentFrame.Load(int3(q,0)).rgb)*.25;
  float3 d=(previousFrame.Load(int3(p,0)).rgb+previousFrame.Load(int3(q.x,p.y,0)).rgb+previousFrame.Load(int3(p.x,q.y,0)).rgb+previousFrame.Load(int3(q,0)).rgb)*.25;
  a|=lum(c)<<(k*8);b|=lum(d)<<(k*8);
 }
 currentLuma[tid.y*ow+tid.x]=a;previousLuma[tid.y*ow+tid.x]=b;
}
)HLSL";
        ComPtr<ID3DBlob> code,errors;
        HRESULT hr=D3DCompile(shader,strlen(shader),"FG packed luma",nullptr,nullptr,"CS","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(FAILED(hr)&&errors)throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Check(hr,"Compile packed luma compute shader");
        Check(d.dev11->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&luma),"Create luma shader");
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=packedW*halfH*sizeof(unsigned);
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_UINT;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements=packedW*halfH;
        for(auto& a:input){
            Check(d.dev11->CreateBuffer(&desc,nullptr,&a.buffer),"Create packed luma buffer");
            Check(d.dev11->CreateUnorderedAccessView(a.buffer.Get(),&ud,&a.uav),"Create luma UAV");
            CuCheck(cudaGraphicsD3D11RegisterResource(&a.resource,a.buffer.Get(),cudaGraphicsRegisterFlagsNone),"Register luma with CUDA");
        }
        desc.ByteWidth=fw*fh*sizeof(int2);desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32G32_SINT;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements=fw*fh;
        for(auto& s:slots){
            s.completionEvent.attach(CreateEventW(nullptr,FALSE,FALSE,nullptr));
            if(!s.completionEvent)throw Error("Create DP4a event",GetLastError());
            Check(d.dev11->CreateBuffer(&desc,nullptr,&s.buffer),"Create DP4a motion buffer");
            Check(d.dev11->CreateShaderResourceView(s.buffer.Get(),&sd,&s.srv),"Create DP4a motion SRV");
            CuCheck(cudaGraphicsD3D11RegisterResource(&s.resource,s.buffer.Get(),cudaGraphicsRegisterFlagsNone),"Register motion output with CUDA");
        }
    }
    UINT64 Completed()const{
        const auto done=flowFence->GetCompletedValue();
        if(done==UINT64_MAX)throw Error("DP4a device removed",owner->dev11->GetDeviceRemovedReason());
        return done;
    }
    void Reap(){
        const UINT64 done=owner->Completed();
        // Capture is read only by D3D11 luma conversion and presentation;
        // CUDA operates exclusively on owned linear buffers.
        for(int i=0;i<kNumCapture;++i)if(owner->retiring[i]&&owner->retiring[i]<=done)owner->ReleaseLease(i);
    }
    bool Matches(uint64_t serial,int c,int p)const{return active>=0&&slots[active].pairSerial==serial&&slots[active].current==c&&slots[active].previous==p;}
    bool Ready(uint64_t serial,int c,int p)const{return Matches(serial,c,p)&&Completed()>=slots[active].submitted;}
    HANDLE ArmCompletionEvent(){
        auto& s=slots[active];
        if(s.completionArmed!=s.submitted){Check(flowFence->SetEventOnCompletion(s.submitted,s.completionEvent.get()),"Arm DP4a event");s.completionArmed=s.submitted;}
        return s.completionEvent.get();
    }
    ID3D11ShaderResourceView* MotionFor(uint64_t serial,int c,int p)const{
        if(!Ready(serial,c,p))throw Error("DP4a motion pair not ready");return slots[active].srv.Get();
    }
    UINT64 ReserveShaderRead(){if(active<0)throw Error("No DP4a motion output");return slots[active].lastRead=++fenceSerial;}
    bool Calculate(int c,int p,bool /*continuous*/,uint64_t serial){
        active=-1;const UINT64 done=Completed();
        if(lastCompute>done)return false; // one scratch pair; never stack CUDA jobs
        int chosen=-1;for(int i=0;i<2;++i)if(slots[i].submitted<=done&&slots[i].lastRead<=done){chosen=i;break;}
        if(chosen<0)return false;
        auto& s=slots[chosen];auto* ctx=owner->ctx11.Get();
        ID3D11ShaderResourceView* views[]{owner->frameSRV[c].Get(),owner->frameSRV[p].Get()};
        ID3D11UnorderedAccessView* uavs[]{input[0].uav.Get(),input[1].uav.Get()};
        ctx->CSSetShader(luma.Get(),nullptr,0);ctx->CSSetShaderResources(0,2,views);ctx->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        ctx->Dispatch((packedW+15)/16,(halfH+7)/8,1);
        ID3D11ShaderResourceView* noViews[2]{};ID3D11UnorderedAccessView* noUavs[2]{};
        ctx->CSSetShaderResources(0,2,noViews);ctx->CSSetUnorderedAccessViews(0,2,noUavs,nullptr);ctx->CSSetShader(nullptr,nullptr,0);
        mapping[0]=input[0].resource;mapping[1]=input[1].resource;mapping[2]=s.resource;
        // Map/unmap orders GPU work across D3D11 and CUDA. These calls may stall
        // on some drivers; no explicit per-frame CPU wait or readback is used.
        CuCheck(cudaGraphicsMapResources(3,mapping,stream),"Map CUDA motion resources");mapped=true;
        void* pointers[3]{};size_t bytes[3]{};
        for(int i=0;i<3;++i)CuCheck(cudaGraphicsResourceGetMappedPointer(&pointers[i],&bytes[i],mapping[i]),"Get mapped DP4a buffer");
        const size_t inputSize=size_t(packedW)*halfH*sizeof(unsigned),outputSize=size_t(fw)*fh*sizeof(int2);
        if(bytes[0]<inputSize||bytes[1]<inputSize||bytes[2]<outputSize)throw Error("CUDA mapped buffer too small");
        CuCheck(fgLaunchDP4a(static_cast<unsigned*>(pointers[0]),static_cast<unsigned*>(pointers[1]),int(packedW),int(halfW),int(halfH),static_cast<int2*>(pointers[2]),int(fw),int(fh),stream),"Launch native DP4a kernel");
        CuCheck(cudaGraphicsUnmapResources(3,mapping,stream),"Unmap CUDA motion resources");mapped=false;
        const UINT64 value=++fenceSerial;
        Check(owner->fenceContext->Signal(flowFence.Get(),value),"Signal DP4a completion");
        s.submitted=lastCompute=value;s.lastRead=0;s.pairSerial=serial;s.current=c;s.previous=p;active=chosen;
        ctx->Flush();owner->retirementNeedsSubmit=false;return true;
    }
    ~Flow11(){
        if(mapped)cudaGraphicsUnmapResources(3,mapping,stream);
        if(stream)cudaStreamSynchronize(stream);
        if(owner)owner->DrainForShutdown();
        for(auto& s:slots)if(s.resource)cudaGraphicsUnregisterResource(s.resource);
        for(auto& a:input)if(a.resource)cudaGraphicsUnregisterResource(a.resource);
        if(stream)cudaStreamDestroy(stream);
    }
};
