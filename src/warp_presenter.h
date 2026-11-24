#pragma once
#include "v124_streamline.h"
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <string>
#include <cstring>
#include <nvOpticalFlowD3D12.h>

namespace fgwarp {
using Microsoft::WRL::ComPtr;
inline std::atomic<bool> requested{false};
inline std::atomic<long long> mouseX{},mouseY{};
inline void check(HRESULT hr,const char* api){
    if(FAILED(hr))throw std::runtime_error(std::string(api)+" HRESULT "+std::to_string(static_cast<unsigned>(hr)));
}
inline void ofcheck(NV_OF_STATUS s,const char* api){
    if(s!=NV_OF_SUCCESS)throw std::runtime_error(std::string(api)+" NvOFA status "+std::to_string(int(s)));
}
struct Handle {HANDLE value{};~Handle(){if(value)CloseHandle(value);}};
struct Presenter {
    bool initialized{},warpEnabled{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11On12Device2> on12;
    ComPtr<IDXGISwapChain3> chain;
    ComPtr<ID3D12CommandAllocator> allocator[2];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> buffers[2];
    ComPtr<ID3D12DescriptorHeap> srvHeap,rtvHeap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> realPipeline,fakePipeline;
    UINT srvStep{},rtvStep{},width{},height{};
    UINT64 serial{},retired[2]{};
    Handle event;
    v124::Camera camera;
    static constexpr double radiansPerCount=-.022*1.5*3.141592653589793/180.;
    static float angle(long long n){return float(std::remainder(n*radiansPerCount,6.283185307179586));}
    void sourcePose(){camera.yaw=angle(mouseX.load());camera.pitch=angle(mouseY.load());}
    void open(IDXGIAdapter* adapter,HWND window,DXGI_SWAP_CHAIN_DESC1 desc,UINT srcW,UINT srcH,
              IDXGISwapChain1** swap,ID3D11Device** d11,ID3D11DeviceContext** c11,bool enableWarp){
        initialized=true;warpEnabled=enableWarp;
        ComPtr<IDXGIFactory2> factory;
        if(warpEnabled){
            v124::FactoryFn factoryFn{};v124::DeviceFn create{};
            v124::Load(factoryFn,create);
            check(factoryFn(IID_PPV_ARGS(&factory)),"Warp factory");
            ComPtr<IDXGIAdapter1> adapter1;check(adapter->QueryInterface(IID_PPV_ARGS(&adapter1)),"Warp adapter");
            check(create(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Warp device");
            v124::Bind(device.Get());v124::CheckAdapter(adapter1.Get());
        }else{
            check(adapter->GetParent(IID_PPV_ARGS(&factory)),"Native12 factory");
            check(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Native12 device");
        }
        D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Native12 graphics queue");
        check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,swap),"Native12 swapchain");
        if(warpEnabled)v124::RequireSwapchainProxy(*swap);
        check((*swap)->QueryInterface(IID_PPV_ARGS(&chain)),"Native12 chain index");
        if(warpEnabled){
            ComPtr<ID3D12Device> native;v124::NativeInterface(device.Get(),reinterpret_cast<void**>(native.GetAddressOf()));
            ComPtr<ID3D12CommandQueue> nativeQueue;
            v124::NativeInterface(queue.Get(),reinterpret_cast<void**>(nativeQueue.GetAddressOf()));
            queue=std::move(nativeQueue);device=native;
        }
        IUnknown* queues[]={queue.Get()};
        check(D3D11On12CreateDevice(device.Get(),D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,queues,1,0,d11,c11,nullptr),"D3D11On12 capture device");
        check((*d11)->QueryInterface(IID_PPV_ARGS(&on12)),"ID3D11On12Device2");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Graphics fence");
        for(auto& a:allocator)check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"Warp allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator[0].Get(),nullptr,IID_PPV_ARGS(&list)),"Warp command list");
        check(list->Close(),"Warp list close");event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event.value)throw std::runtime_error("Warp fence event creation failed");
        width=desc.Width;height=desc.Height;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=6;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srvHeap)),"Warp SRV heap");
        hd={};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
        check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvHeap)),"Warp RTV heap");
        srvStep=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtvStep=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for(UINT i=0;i<2;++i){check(chain->GetBuffer(i,IID_PPV_ARGS(&buffers[i])),"Native swapchain buffer");auto h=rtvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*rtvStep;device->CreateRenderTargetView(buffers[i].Get(),nullptr,h);}
        if(warpEnabled){v124::InitializePlanes(device.Get(),queue.Get(),width,height,srcW,srcH,2,UINT(1000.0*srcW/srcH));
        camera.aspect=float(srcW)/srcH;camera.verticalFov=73.74f;v124::Enable();}
    }
    void shaders(ID3DBlob* vs,ID3DBlob* ps,ID3DBlob* rvs,ID3DBlob* rps){
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=3;range.BaseShaderRegister=0;
        range.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,12};params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxAnisotropy=1;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
        rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature,errors;
        check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors),"Warp serialize root");
        check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)),"Warp root");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();p.SampleMask=UINT_MAX;
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
        p.BlendState.RenderTarget[0]={FALSE,FALSE,D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,D3D12_LOGIC_OP_NOOP,D3D12_COLOR_WRITE_ENABLE_ALL};
        p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        p.DepthStencilState.StencilEnable=FALSE;p.DepthStencilState.StencilReadMask=D3D12_DEFAULT_STENCIL_READ_MASK;p.DepthStencilState.StencilWriteMask=D3D12_DEFAULT_STENCIL_WRITE_MASK;
        p.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;p.SampleDesc.Count=1;
        p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        check(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&fakePipeline)),"Native12 extrapolation PSO");
        p.VS={rvs->GetBufferPointer(),rvs->GetBufferSize()};p.PS={rps->GetBufferPointer(),rps->GetBufferSize()};
        check(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&realPipeline)),"Native12 real PSO");
    }
    UINT64 signal(){check(queue->Signal(fence.Get(),++serial),"Signal native12 graphics");return serial;}
    void wait(UINT64 n){
        if(!n)return;
        UINT64 done=fence->GetCompletedValue();
        if(done==UINT64_MAX)throw std::runtime_error("Native12 device removed");
        if(done<n){check(fence->SetEventOnCompletion(n,event.value),"Native12 fence event");if(WaitForSingleObject(event.value,3000)!=WAIT_OBJECT_0)throw std::runtime_error("Native12 GPU timed out");}
    }
    void drain(){if(queue&&fence&&event.value)wait(signal());}
    ComPtr<ID3D12Resource> unwrap(ID3D11Texture2D* source,ID3D11DeviceContext* c11){
        ComPtr<ID3D12Resource> r;
        check(on12->UnwrapUnderlyingResource(source,queue.Get(),IID_PPV_ARGS(&r)),"Strict native12 WGC unwrap");
        // Translation-layer submissions can be deferred. Submit before external OFA/graphics reads.
        c11->Flush();return r;
    }
    void giveBack(ID3D11Texture2D* source,UINT64 done){
        ID3D12Fence* ff[]={fence.Get()};UINT64 values[]={done};
        check(on12->ReturnUnderlyingResource(source,1,values,ff),"Return WGC native12 surface");
    }
    void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&t);
    }
    static void sampleRealPose(v124::Camera& c){MSG input{};for(unsigned k=0;k<32&&PeekMessageW(&input,nullptr,WM_INPUT,WM_INPUT,PM_REMOVE);++k)DispatchMessageW(&input);c.lateYaw=angle(mouseX.load());c.latePitch=angle(mouseY.load());}
    static void samplePredictedPose(v124::Camera& c){c.lateYaw=c.yaw;c.latePitch=c.pitch;}
    bool slotReady()const{const UINT64 completed=fence->GetCompletedValue();if(completed==UINT64_MAX)throw std::runtime_error("Native12 device removed");return completed>=retired[chain->GetCurrentBackBufferIndex()];}
    void render(ID3D12Resource* current,ID3D12Resource* previous,ID3D12Resource* motion,const void* params,
                bool real,UINT flags,UINT w,UINT h,UINT radius,UINT feather){
        UINT i=chain->GetCurrentBackBufferIndex();if(!slotReady())throw std::runtime_error("Native12 graphics slot not ready");
        check(allocator[i]->Reset(),"Native12 allocator reset");check(list->Reset(allocator[i].Get(),real?realPipeline.Get():fakePipeline.Get()),"Native12 list reset");
        auto cpu=srvHeap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(i)*3*srvStep;
        auto gpu=srvHeap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(i)*3*srvStep;
        ID3D12Resource* inputs[]={current,motion,previous};
        for(UINT k=0;k<3;++k){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=k==1?DXGI_FORMAT_R16G16_SINT:DXGI_FORMAT_B8G8R8A8_UNORM;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;device->CreateShaderResourceView(inputs[k],&v,cpu);cpu.ptr+=srvStep;}
        const auto read=D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(current,D3D12_RESOURCE_STATE_COMMON,read);
        if(!real&&previous!=current)barrier(previous,D3D12_RESOURCE_STATE_COMMON,read);
        if(!real)barrier(motion,D3D12_RESOURCE_STATE_COMMON,read);
        auto* back=buffers[i].Get();
        barrier(back,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};D3D12_RECT sc{0,0,LONG(width),LONG(height)};
        auto rt=rtvHeap->GetCPUDescriptorHandleForHeapStart();rt.ptr+=SIZE_T(i)*rtvStep;
        list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);list->OMSetRenderTargets(1,&rt,FALSE,nullptr);
        ID3D12DescriptorHeap* heaps[]={srvHeap.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetGraphicsRootSignature(root.Get());list->SetGraphicsRootDescriptorTable(0,gpu);
        if(!real)list->SetGraphicsRoot32BitConstants(1,12,params,0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        barrier(back,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        barrier(current,read,D3D12_RESOURCE_STATE_COMMON);
        if(!real&&previous!=current)barrier(previous,read,D3D12_RESOURCE_STATE_COMMON);
        if(!real)barrier(motion,read,D3D12_RESOURCE_STATE_COMMON);
        check(list->Close(),"Native12 draw close");ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        if(warpEnabled){
            v124::Frame frame{};frame.back=back;frame.index=i;frame.sourceWidth=w;frame.sourceHeight=h;frame.centerRadiusPx=radius;frame.centerFeatherPx=feather;frame.camera=camera;
            v124::Prepare(frame);
            v124::LatchCamera(camera,real?&sampleRealPose:&samplePredictedPose);
            check(chain->Present(0,flags),"Native12 Frame Warp Present");v124::PresentEnd();
        }else check(chain->Present(0,flags),"Native12 direct Present");
        retired[i]=signal();
    }
    ~Presenter(){if(initialized){try{drain();}catch(...){}if(warpEnabled){v124::Disable();v124::ReleasePlanes();}for(auto& b:buffers)b.Reset();chain.Reset();list.Reset();for(auto& a:allocator)a.Reset();on12.Reset();queue.Reset();device.Reset();if(warpEnabled)v124::Shutdown();}}
};

struct NativeFlow {
    Presenter* p{};
    HMODULE library{};
    NV_OF_D3D12_API_FUNCTION_LIST funcs{};
    NvOFHandle handle{};
    ComPtr<ID3D12Fence> done;
    ComPtr<ID3D12Resource> motion;
    NvOFGPUBufferHandle output{};
    UINT64 serial{},flowValue{};
    Handle event;
    void init(Presenter& presenter,UINT w,UINT h){
        p=&presenter;library=LoadLibraryW(L"nvofapi64.dll");if(!library)throw std::runtime_error("NVOFA DLL missing");
        auto entry=reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(GetProcAddress(library,"NvOFAPICreateInstanceD3D12"));
        if(!entry)throw std::runtime_error("NVOFA D3D12 entry missing");
        ofcheck(entry(NV_OF_API_VERSION,&funcs),"NvOFA D3D12 API");
        if(!funcs.nvCreateOpticalFlowD3D12||!funcs.nvOFInit||!funcs.nvOFRegisterResourceD3D12||!funcs.nvOFUnregisterResourceD3D12||!funcs.nvOFExecuteD3D12||!funcs.nvOFDestroy)throw std::runtime_error("Incomplete NvOFA D3D12 API");
        ofcheck(funcs.nvCreateOpticalFlowD3D12(p->device.Get(),&handle),"NvOFA native12 create");
        NV_OF_INIT_PARAMS in{};in.width=w;in.height=h;in.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;in.mode=NV_OF_MODE_OPTICALFLOW;in.perfLevel=NV_OF_PERF_LEVEL_FAST;in.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;
        in.enableOutputCost=NV_OF_FALSE;in.enableExternalHints=NV_OF_FALSE;ofcheck(funcs.nvOFInit(handle,&in),"NvOFA native12 init");
        check(p->device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)),"NvOFA fence");
        event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event.value)throw std::runtime_error("NvOFA event creation failed");
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;r.Width=(w+3)/4;r.Height=(h+3)/4;r.DepthOrArraySize=r.MipLevels=1;r.Format=DXGI_FORMAT_R16G16_SINT;r.SampleDesc.Count=1;r.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        check(p->device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&r,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&motion)),"NvOFA native12 motion texture");
        registerResource(motion.Get(),output);
    }
    UINT64 completed()const{if(!done)return 0;UINT64 n=done->GetCompletedValue();if(n==UINT64_MAX)throw std::runtime_error("NvOFA device removed");return n;}
    bool ready()const{return flowValue!=0&&completed()>=flowValue;}
    void registerResource(ID3D12Resource* r,NvOFGPUBufferHandle& dst){
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 a{};a.resource=r;a.hOFGpuBuffer=&dst;a.inputFencePoint={p->fence.Get(),p->serial};a.outputFencePoint={done.Get(),serial+1};
        ofcheck(funcs.nvOFRegisterResourceD3D12(handle,&a),"Strict native12 NvOFA registration");++serial;
    }
    void unregister(NvOFGPUBufferHandle& h){
        if(h){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 a{};a.hOFGpuBuffer=h;ofcheck(funcs.nvOFUnregisterResourceD3D12(&a),"NvOFA native12 unregister");h=nullptr;}
    }
    void execute(NvOFGPUBufferHandle cur,NvOFGPUBufferHandle prev,bool continuous){
        NV_OF_FENCE_POINT input[]={{p->fence.Get(),p->serial},{done.Get(),serial}};
        NV_OF_FENCE_POINT outputPoint{done.Get(),serial+1};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 a{};a.inputFrame=cur;a.referenceFrame=prev;a.disableTemporalHints=continuous?NV_OF_FALSE:NV_OF_TRUE;a.numFencePoints=2;a.fencePoint=input;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 b{};b.outputBuffer=output;b.fencePoint=&outputPoint;
        ofcheck(funcs.nvOFExecuteD3D12(handle,&a,&b),"NvOFA native12 execute");flowValue=++serial;
    }
    void drain(){if(!done||!event.value||completed()>=serial)return;check(done->SetEventOnCompletion(serial,event.value),"NvOFA drain event");if(WaitForSingleObject(event.value,3000)!=WAIT_OBJECT_0)throw std::runtime_error("NvOFA drain timeout");}
    ~NativeFlow(){try{drain();unregister(output);}catch(...){}if(handle&&funcs.nvOFDestroy)funcs.nvOFDestroy(handle);if(library)FreeLibrary(library);}
};
}
