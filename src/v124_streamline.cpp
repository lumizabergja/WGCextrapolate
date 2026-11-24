#include "v124_streamline.h"
#include <wrl/client.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <fstream>
#include <atomic>
// Isolate the legacy SDK definitions from the main presenter's 2.14 headers.
#define sl sl27
#include "../vendor/streamline/sl.h"
#include "../vendor/streamline/sl_reflex.h"
#include "../vendor/streamline/sl_pcl.h"
#undef sl
namespace v124 {
using Microsoft::WRL::ComPtr;
namespace {
HMODULE module{};
std::atomic<const wchar_t*> stage{L"startup"};
PFun_slGetNativeInterface* nativeInterface{};
PFun_slInit* init{};PFun_slShutdown* shutdown{};
PFun_slSetD3DDevice* bind{};PFun_slGetFeatureFunction* getFeature{};
PFun_slIsFeatureSupported* supported{};PFun_slIsFeatureLoaded* isLoaded{};
PFun_slGetFeatureRequirements* requirements{};PFun_slGetNewFrameToken* newToken{};
PFun_slSetConstants* constants{};PFun_slSetTag* setTag{};
PFun_slReflexSetOptions* reflexOptions{};PFun_slReflexSetCameraData* setCamera{};
PFun_slPCLSetMarker* marker{};
// V1 options: BaseStructure (32 bytes), 4-byte legacy field, bool at +36.
// The 3DMark production plugin copies only the viewport and reads the bool
// and version. V1 deliberately avoids unknown V2 fields; no private GUID is
// guessed. The exact signed plugin SHA256 is checked before using this ABI.
struct alignas(8) Options27 {
    void* next{};sl27::StructType reservedType{};size_t structVersion{1};
    uint32_t legacyZero{};uint8_t latewarpActive{};uint8_t padding[3]{};
};
static_assert(sizeof(Options27)==40 && offsetof(Options27,latewarpActive)==36 && offsetof(Options27,structVersion)==24);
static_assert(sizeof(sl27::ViewportHandle)==40);
using SetOptionsFn=sl27::Result(const sl27::ViewportHandle&,const Options27&);
SetOptionsFn* setOptions{};
sl27::ViewportHandle viewport{0};sl27::FrameToken* currentToken{};
uint32_t frameId=1;bool active=false,initialized=false,first=true;
std::array<float,16> previousSource{};
ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
ComPtr<ID3D12Fence> fence;HANDLE eventHandle{};UINT64 serial=0;
struct Slot {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> color,mask;UINT64 fenceValue{};};
std::vector<Slot> slots;ComPtr<ID3D12Resource> depth,zeroMotion,fixedMask,zeroUI;
UINT width{},height{},motionW{},motionH{};
constexpr D3D12_RESOURCE_STATES readState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
void Check(sl27::Result r,const char* name){if(r!=sl27::Result::eOk)throw std::runtime_error(std::string("V124 ")+name+" Streamline result "+std::to_string(int(r)));}
void HR(HRESULT hr,const char* name){if(FAILED(hr))throw std::runtime_error(std::string("V124 ")+name+" HRESULT "+std::to_string(unsigned(hr)));}
template<class T>void Export(T*& f,const char* name){f=reinterpret_cast<T*>(GetProcAddress(module,name));if(!f)throw std::runtime_error(std::string("V124 missing NVIDIA export: ")+name);}
template<class T>void Feature(T*& f,sl27::Feature id,const char* name){void* p{};Check(getFeature(id,name,p),name);if(!p)throw std::runtime_error(std::string("V124 null NVIDIA feature: ")+name);f=reinterpret_cast<T*>(p);}
std::string SHA(const std::filesystem::path& p){
    std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("V124 matched NVIDIA runtime file missing");
    BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};DWORD n=0,cb=0;std::vector<BYTE> obj;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("V124 SHA256 init");
    if(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&n),sizeof(n),&cb,0)<0){BCryptCloseAlgorithmProvider(alg,0);throw std::runtime_error("V124 SHA256 size");}
    obj.resize(n);if(BCryptCreateHash(alg,&hash,obj.data(),n,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(alg,0);throw std::runtime_error("V124 SHA256 hash");}
    BYTE data[65536],digest[32];while(f){f.read(reinterpret_cast<char*>(data),sizeof(data));if(f.gcount()>0&&BCryptHashData(hash,data,ULONG(f.gcount()),0)<0){BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);throw std::runtime_error("V124 SHA256 read");}}
    const auto rc=BCryptFinishHash(hash,digest,32,0);BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);if(rc<0)throw std::runtime_error("V124 SHA256 finish");
    constexpr char hex[]="0123456789abcdef";std::string out;for(BYTE b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;
}
void Verify(const std::filesystem::path& p,const char* hash){if(SHA(p)!=hash)throw std::runtime_error("V124 matched NVIDIA runtime SHA256 mismatch: "+p.filename().string());}
std::array<float,16> CameraMatrix(float yaw,float pitch){const float cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);return {cy,sy*sp,-sy*cp,0,0,cp,sp,0,sy,-cy*sp,cy*cp,0,0,0,0,1};}
std::array<float,16> Projection(const Camera& c){const float aspect=std::clamp(c.aspect,.5f,4.f);float vf=std::clamp(c.verticalFov,25.f,130.f)*0.017453292519943295f;if(c.horizontalFov>1)vf=2*std::atan(std::tan(std::clamp(c.horizontalFov,35.f,140.f)*.00872664625997165f)/aspect);const float f=1/std::tan(vf*.5f);constexpr float n=.1f,z=1000.f;return {f/aspect,0,0,0,0,f,0,0,0,0,z/(z-n),1,0,0,-n*z/(z-n),0};}
sl27::float4x4 Mat(const std::array<float,16>& a){sl27::float4x4 m{};for(UINT i=0;i<4;++i)m[i]={a[i*4],a[i*4+1],a[i*4+2],a[i*4+3]};return m;}
std::array<float,16> Identity(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
std::array<float,16> Multiply(const std::array<float,16>& a,const std::array<float,16>& b){std::array<float,16> r{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)r[4*i+j]+=a[4*i+k]*b[4*k+j];return r;}
std::array<float,16> Transpose(const std::array<float,16>& a){std::array<float,16> r{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)r[4*i+j]=a[4*j+i];return r;}
std::array<float,16> InverseProjection(const std::array<float,16>& a){return {1/a[0],0,0,0,0,1/a[5],0,0,0,0,0,1/a[14],0,0,1,-a[10]/a[14]};}
void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&x);}
void Plane(ComPtr<ID3D12Resource>& r,UINT w,UINT h,DXGI_FORMAT format,D3D12_RESOURCE_STATES state){D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"create matched Latewarp resource");}
void Wait(UINT64 v){if(v&&fence->GetCompletedValue()<v){HR(fence->SetEventOnCompletion(v,eventHandle),"Latewarp fence event");if(WaitForSingleObject(eventHandle,3000)!=WAIT_OBJECT_0)throw std::runtime_error("V124 Latewarp resource fence timed out");}}
void Upload(ID3D12GraphicsCommandList* list,ID3D12Resource* dst,const std::vector<uint8_t>& bytes,UINT pixelBytes,std::vector<ComPtr<ID3D12Resource>>& uploads){
    auto d=dst->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 row{},size{};device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&row,&size);
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_UPLOAD;hp.CreationNodeMask=hp.VisibleNodeMask=1;D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=size;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> u;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&u)),"upload plane");void* v{};D3D12_RANGE range{0,0};HR(u->Map(0,&range,&v),"upload map");for(UINT y=0;y<rows;++y)memcpy(static_cast<uint8_t*>(v)+fp.Offset+size_t(y)*fp.Footprint.RowPitch,bytes.data()+size_t(y)*d.Width*pixelBytes,size_t(d.Width)*pixelBytes);u->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=u.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=fp;b.pResource=dst;b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&b,0,0,0,&a,nullptr);Transition(list,dst,D3D12_RESOURCE_STATE_COPY_DEST,readState);uploads.push_back(std::move(u));
}
sl27::Resource Resource(ID3D12Resource* r,D3D12_RESOURCE_STATES state){sl27::Resource s(sl27::ResourceType::eTex2d,r,state);auto d=r->GetDesc();s.width=UINT(d.Width);s.height=d.Height;s.nativeFormat=d.Format;s.mipLevels=d.MipLevels;s.arrayLayers=d.DepthOrArraySize;s.flags=0;s.usage=0;return s;}
sl27::Extent Extent(ID3D12Resource* r){auto d=r->GetDesc();return {0,0,UINT(d.Width),d.Height};}
void CameraData(sl27::FrameToken& t,const std::array<float,16>& target,const std::array<float,16>& source,const std::array<float,16>& proj){sl27::ReflexCameraData data{};data.worldToViewMatrix=Mat(target);data.viewToClipMatrix=Mat(proj);data.prevRenderedWorldToViewMatrix=Mat(source);data.prevRenderedViewToClipMatrix=Mat(proj);Check(setCamera(viewport,t,data),"slReflexSetCameraData");}
}
void (*stageCallback)(const wchar_t*){};
void Stage(const wchar_t* p) noexcept {if(stageCallback)stageCallback(p);stage.store(p,std::memory_order_relaxed);}
const wchar_t* LastStage() noexcept {return stage.load(std::memory_order_relaxed);}
void NativeInterface(IUnknown* proxy,void** native){if(!proxy||!native||!nativeInterface)throw std::runtime_error("V125 native interface unavailable");*native=nullptr;Check(nativeInterface(proxy,native),"slGetNativeInterface");if(!*native)throw std::runtime_error("V125 null native interface");}
void RequireSwapchainProxy(IDXGISwapChain* proxy){Stage(L"Streamline swapchain proxy validation");ComPtr<IDXGISwapChain> native;NativeInterface(proxy,reinterpret_cast<void**>(native.GetAddressOf()));if(native.Get()==proxy)throw std::runtime_error("V125 NVIDIA Streamline did not create a swapchain proxy; no fallback");}
void Load(FactoryFn& f,DeviceFn& d){
    Stage(L"verify isolated NVIDIA runtime");
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);auto dir=std::filesystem::path(path).parent_path();
    dir/=L"FrameWarpRuntime";
    Verify(dir/L"sl.latewarp.dll","e009007c8971e2c45f6861e71267cd76edc239e11bbbde346abe9c273f33c789");
    Verify(dir/L"nvngx_latewarp.dll","cfa07e2849ea8b14b98cccecf7b71e2ae41acc045c5b90eaf194c248892c5063");
    Verify(dir/L"sl.common.dll","2630a567a9142512afee067522c9147766bc218ea4df748de2d2486801266979");
    Verify(dir/L"sl.interposer.dll","5032e5f5e76260815ac309d58d9bd4bf6da5d55873c216c7141c3f192ed38105");
    Verify(dir/L"sl.reflex.dll","270e79642349941024e4a7f27838489cb59dd31a0ac65fe34cedff7a3de8b09b");
    Verify(dir/L"sl.pcl.dll","d5a9d2c3d86135100cb9c9de9fb34ced8cf4530cf713da56211b5d0a308791e8");
    Stage(L"load signed Streamline interposer");
    if(!module)module=LoadLibraryExW((dir/L"sl.interposer.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!module)throw std::runtime_error("V124 cannot load matched Streamline 2.7 interposer");
    Export(nativeInterface,"slGetNativeInterface");Export(init,"slInit");Export(shutdown,"slShutdown");Export(bind,"slSetD3DDevice");Export(getFeature,"slGetFeatureFunction");Export(supported,"slIsFeatureSupported");Export(isLoaded,"slIsFeatureLoaded");Export(requirements,"slGetFeatureRequirements");Export(newToken,"slGetNewFrameToken");Export(constants,"slSetConstants");Export(setTag,"slSetTag");Export(f,"CreateDXGIFactory1");Export(d,"D3D12CreateDevice");
    auto absolute=dir.wstring();const wchar_t* paths[]={absolute.c_str()};const sl27::Feature features[]={sl27::kFeatureReflex,sl27::kFeaturePCL,sl27::kFeatureLatewarp};sl27::Preferences p{};p.pathsToPlugins=paths;p.numPathsToPlugins=1;p.logLevel=sl27::LogLevel::eOff;p.pathToLogsAndData=nullptr;p.flags=sl27::PreferenceFlags::eDisableCLStateTracking|sl27::PreferenceFlags::eUseDXGIFactoryProxy;p.featuresToLoad=features;p.numFeaturesToLoad=3;p.engine=sl27::EngineType::eCustom;p.engineVersion="FGFW V124 native Latewarp";p.projectId="e86d5d1d-b886-47ac-a90d-48708688358b";p.renderAPI=sl27::RenderAPI::eD3D12;
    Stage(L"slInit 2.7");constexpr uint64_t exactVersion=(uint64_t(2)<<48)|(uint64_t(7)<<32)|0xfedc;Check(init(p,exactVersion),"slInit matched 2.7.0");initialized=true;
}
void Bind(ID3D12Device* d){Stage(L"slSetD3DDevice native");ComPtr<ID3D12Device> native;NativeInterface(d,reinterpret_cast<void**>(native.GetAddressOf()));Check(bind(native.Get()),"slSetD3DDevice native");}
void CheckAdapter(IDXGIAdapter1* a){Stage(L"Streamline feature support and API resolution");DXGI_ADAPTER_DESC1 desc{};HR(a->GetDesc1(&desc),"adapter LUID");sl27::AdapterInfo ai{};ai.deviceLUID=reinterpret_cast<uint8_t*>(&desc.AdapterLuid);ai.deviceLUIDSizeInBytes=sizeof(LUID);Check(supported(sl27::kFeatureLatewarp,ai),"Latewarp support");bool loaded=false;Check(isLoaded(sl27::kFeatureLatewarp,loaded),"Latewarp loaded");if(!loaded)throw std::runtime_error("V124 NVIDIA Latewarp plugin not loaded; no fallback");sl27::FeatureRequirements req{};req.structVersion=1;Check(requirements(sl27::kFeatureLatewarp,req),"Latewarp requirements");if(!(uint32_t(req.flags)&uint32_t(sl27::FeatureRequirementFlags::eD3D12Supported)))throw std::runtime_error("V124 Latewarp DX12 unsupported");
    Feature(setOptions,sl27::kFeatureLatewarp,"slLatewarpSetOptions");Feature(reflexOptions,sl27::kFeatureReflex,"slReflexSetOptions");Feature(setCamera,sl27::kFeatureReflex,"slReflexSetCameraData");Feature(marker,sl27::kFeaturePCL,"slPCLSetMarker");sl27::ReflexOptions o{};o.mode=sl27::ReflexMode::eLowLatencyWithBoost;Check(reflexOptions(o),"Reflex On+Boost");
}
void Enable(){Stage(L"slLatewarpSetOptions ON");if(!setOptions)throw std::runtime_error("V125 Latewarp options missing");Options27 o{};o.latewarpActive=1;Check(setOptions(viewport,o),"slLatewarpSetOptions ON");active=true;first=true;frameId=1;}
void Disable() noexcept{Stage(L"slLatewarpSetOptions OFF");if(active&&setOptions){Options27 o{};setOptions(viewport,o);}active=false;}
void InitializePlanes(ID3D12Device* d,ID3D12CommandQueue* q,UINT w,UINT h,UINT mw,UINT mh,UINT count,UINT sourceAspectMilli){Stage(L"allocate native Latewarp resource planes");if(!count||!w||!h||!d||!q)throw std::runtime_error("V125 invalid Latewarp dimensions or device");device=d;queue=q;width=w;height=h;motionW=std::max(1u,mw);motionH=std::max(1u,mh);slots.resize(count);HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Latewarp plane fence");eventHandle=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!eventHandle)throw std::runtime_error("V124 plane event");
    for(auto& slot:slots){HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocator)),"Latewarp allocator");HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(&slot.list)),"Latewarp list");HR(slot.list->Close(),"Latewarp initial close");Plane(slot.color,w,h,DXGI_FORMAT_B8G8R8A8_UNORM,readState);Plane(slot.mask,w,h,DXGI_FORMAT_R8_UNORM,readState);}
    Plane(depth,motionW,motionH,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);Plane(zeroMotion,motionW,motionH,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);Plane(fixedMask,w,h,DXGI_FORMAT_R8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST);Plane(zeroUI,w,h,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST);
    auto& s=slots[0];HR(s.allocator->Reset(),"plane allocator");HR(s.list->Reset(s.allocator.Get(),nullptr),"plane list");std::vector<ComPtr<ID3D12Resource>> u;std::vector<uint8_t> values(size_t(motionW)*motionH*4);const float dep=.999f;for(size_t i=0;i<values.size();i+=4)memcpy(values.data()+i,&dep,4);Upload(s.list.Get(),depth.Get(),values,4,u);std::fill(values.begin(),values.end(),0);Upload(s.list.Get(),zeroMotion.Get(),values,4,u);values.assign(size_t(w)*h,0);const float aspect=float(sourceAspectMilli)/1000.f;for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){float u0=(x+.5f)/w,v=(y+.5f)/h;float cx=(u0-.5f)*aspect,cy=v-.5f;if(v<=.055f||v>=.88f||cx*cx+cy*cy<=.005f*.005f)values[size_t(y)*w+x]=255;}Upload(s.list.Get(),fixedMask.Get(),values,1,u);values.assign(size_t(w)*h*4,0);Upload(s.list.Get(),zeroUI.Get(),values,4,u);HR(s.list->Close(),"plane close");ID3D12CommandList* lists[]={s.list.Get()};queue->ExecuteCommandLists(1,lists);HR(queue->Signal(fence.Get(),++serial),"plane signal");Wait(serial);
}
void Prepare(const Frame& f){Stage(L"Latewarp native resource copy");if(!active||!f.back||f.index>=slots.size())throw std::runtime_error("V124 Latewarp missing active resources");auto& s=slots[f.index];Wait(s.fenceValue);HR(s.allocator->Reset(),"Latewarp allocator reuse");HR(s.list->Reset(s.allocator.Get(),nullptr),"Latewarp list reuse");auto* l=s.list.Get();
    // Separate color copy avoids aliasing the native swapchain resource while
    // the NVIDIA plugin owns the final Present. Mask and compact flow remain GPU-resident.
    Transition(l,f.back,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);Transition(l,s.color.Get(),readState,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyResource(s.color.Get(),f.back);Transition(l,s.color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,readState);Transition(l,f.back,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
    Stage(L"Latewarp frame token");Check(newToken(currentToken,&frameId),"slGetNewFrameToken");if(!currentToken)throw std::runtime_error("V124 current frame token null");
    const auto src=CameraMatrix(f.camera.yaw,f.camera.pitch),projection=Projection(f.camera),inv=InverseProjection(projection);if(first){previousSource=src;if(frameId==1)CameraData(*currentToken,src,src,projection);}
    Check(marker(sl27::PCLMarker::eSimulationStart,*currentToken),"Reflex simulation start");Check(marker(sl27::PCLMarker::eSimulationEnd,*currentToken),"Reflex simulation end");
    Stage(L"Latewarp frame constants");sl27::Constants c{};c.cameraViewToClip=Mat(projection);c.clipToCameraView=Mat(inv);c.clipToLensClip=Mat(Identity());c.clipToPrevClip=Mat(Multiply(Multiply(Multiply(inv,Transpose(src)),previousSource),projection));c.prevClipToClip=Mat(Multiply(Multiply(Multiply(inv,Transpose(previousSource)),src),projection));c.jitterOffset={0,0};c.mvecScale={1.f/std::max(1u,f.sourceWidth),1.f/std::max(1u,f.sourceHeight)};c.cameraPinholeOffset={0,0};c.cameraPos={0,0,0};c.cameraUp={src[1],src[5],src[9]};c.cameraRight={src[0],src[4],src[8]};c.cameraFwd={src[2],src[6],src[10]};c.cameraNear=.1f;c.cameraFar=1000.f;c.cameraFOV=2*std::atan(1.f/projection[5]);c.cameraAspectRatio=f.camera.aspect;c.motionVectorsInvalidValue=65504.f;c.depthInverted=sl27::Boolean::eFalse;c.cameraMotionIncluded=sl27::Boolean::eTrue;c.motionVectors3D=sl27::Boolean::eFalse;c.reset=first?sl27::Boolean::eTrue:sl27::Boolean::eFalse;c.orthographicProjection=sl27::Boolean::eFalse;c.motionVectorsDilated=sl27::Boolean::eFalse;c.motionVectorsJittered=sl27::Boolean::eFalse;Check(constants(c,*currentToken,viewport),"slSetConstants");
    auto* mask=fixedMask.Get();auto* mv=f.motion?f.motion:zeroMotion.Get();auto back=Resource(f.back,D3D12_RESOURCE_STATE_PRESENT),color=Resource(s.color.Get(),readState),dep=Resource(depth.Get(),readState),motion=Resource(mv,f.motion?D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE):readState),ui=Resource(zeroUI.Get(),readState),protection=Resource(mask,readState);auto ce=Extent(s.color.Get()),de=Extent(depth.Get()),me=Extent(mv),pe=Extent(mask);
    sl27::ResourceTag tags[]={{&back,sl27::kBufferTypeBackbuffer,sl27::eValidUntilPresent,&ce},{&color,sl27::kBufferTypeHUDLessColor,sl27::eValidUntilPresent,&ce},{&dep,sl27::kBufferTypeDepth,sl27::eValidUntilPresent,&de},{&motion,sl27::kBufferTypeMotionVectors,sl27::eValidUntilPresent,&me},{&ui,sl27::kBufferTypeUIColorAndAlpha,sl27::eValidUntilPresent,&ce},{&protection,sl27::kBufferTypeNoWarpMask,sl27::eValidUntilPresent,&pe}};
    Stage(L"Latewarp resource tags");Check(setTag(viewport,tags,UINT(std::size(tags)),l),"slSetTag Latewarp resources");
    Check(marker(sl27::PCLMarker::eRenderSubmitStart,*currentToken),"Reflex render submit start");HR(l->Close(),"Latewarp resource list close");ID3D12CommandList* lists[]={l};queue->ExecuteCommandLists(1,lists);Check(marker(sl27::PCLMarker::eRenderSubmitEnd,*currentToken),"Reflex render submit end");HR(queue->Signal(fence.Get(),++serial),"Latewarp copy fence");s.fenceValue=serial;
    previousSource=src;first=false;
}
// Native 2.7 binary reads camera frame N+1 in its Present evaluator. Submit
// that token with the actual new pose and the exact source pose in the
// prevRendered fields, AFTER preparing the source and immediately before Present.
void LatchCamera(const Camera& source,LatePoseSampler sample){Stage(L"Latewarp next camera token");if(!active||!currentToken)throw std::runtime_error("V124 missing current token");uint32_t next=frameId+1;sl27::FrameToken* t{};Check(newToken(t,&next),"next Reflex camera token");if(!t)throw std::runtime_error("V124 next frame token null");Camera c=source;if(sample)sample(c);CameraData(*t,CameraMatrix(c.lateYaw,c.latePitch),CameraMatrix(c.yaw,c.pitch),Projection(c));Check(marker(sl27::PCLMarker::ePresentStart,*currentToken),"Reflex Present start");}
void PresentEnd(){Stage(L"Latewarp Present end");if(currentToken){Check(marker(sl27::PCLMarker::ePresentEnd,*currentToken),"Reflex Present end");currentToken=nullptr;++frameId;}}
void ResetCameraHistory() noexcept{first=true;}
void ReleasePlanes() noexcept{try{if(fence&&queue){HR(queue->Signal(fence.Get(),++serial),"Latewarp drain");Wait(serial);}}catch(...){}slots.clear();depth.Reset();zeroMotion.Reset();fixedMask.Reset();zeroUI.Reset();fence.Reset();queue.Reset();device.Reset();if(eventHandle){CloseHandle(eventHandle);eventHandle=nullptr;}serial=0;}
void Shutdown() noexcept{Disable();ReleasePlanes();if(initialized&&shutdown)shutdown();initialized=false;// The engine process exits after Stop. Keep the interposer resident until
    // all local COM proxies have been released by their owning scopes.
    // FreeLibrary here in V124 could leave live proxy vtables pointing into unmapped memory.
    currentToken=nullptr;setOptions=nullptr;}
}
