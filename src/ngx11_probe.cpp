#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <filesystem>
#include <cstdio>
#include <vector>
#include <array>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <cstring>
using Microsoft::WRL::ComPtr;
constexpr UINT W=1920,H=1080;
void hr(HRESULT r,const char* s){printf("%s: 0x%08X\n",s,unsigned(r));if(FAILED(r))throw std::runtime_error(s);}
void ngx(NVSDK_NGX_Result r,const char* s){printf("%s: 0x%08X\n",s,unsigned(r));if(!NVSDK_NGX_SUCCEED(r))throw std::runtime_error(s);}
struct Session{
 ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;NVSDK_NGX_Parameter* p{};NVSDK_NGX_Handle* h{};bool init{};
 ~Session(){if(h)NVSDK_NGX_D3D11_ReleaseFeature(h);if(p)NVSDK_NGX_D3D11_DestroyParameters(p);if(init)NVSDK_NGX_D3D11_Shutdown1(dev.Get());}
};
ComPtr<ID3D11Texture2D> texture(ID3D11Device* d,DXGI_FORMAT fmt,UINT bind,const void* data=nullptr,UINT pitch=0){
 D3D11_TEXTURE2D_DESC t{};t.Width=W;t.Height=H;t.MipLevels=t.ArraySize=1;t.Format=fmt;t.SampleDesc.Count=1;t.Usage=D3D11_USAGE_DEFAULT;t.BindFlags=bind;
 D3D11_SUBRESOURCE_DATA initial{data,pitch,0};ComPtr<ID3D11Texture2D> result;hr(d->CreateTexture2D(&t,data?&initial:nullptr,&result),"Create native DX11 texture");return result;
}
std::vector<uint32_t> read(Session& s,ID3D11Texture2D* src){
 D3D11_TEXTURE2D_DESC d{};src->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> dst;hr(s.dev->CreateTexture2D(&d,nullptr,&dst),"Readback staging");s.ctx->CopyResource(dst.Get(),src);
 D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> ev;hr(s.dev->CreateQuery(&q,&ev),"GPU completion query");s.ctx->End(ev.Get());s.ctx->Flush();
 ULONGLONG deadline=GetTickCount64()+8000;HRESULT r;
 while((r=s.ctx->GetData(ev.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE){if(GetTickCount64()>=deadline)throw std::runtime_error("GPU completion timeout");Sleep(1);}hr(r,"GPU completion");
 D3D11_MAPPED_SUBRESOURCE m{};hr(s.ctx->Map(dst.Get(),0,D3D11_MAP_READ,0,&m),"Readback");std::vector<uint32_t> pixels(size_t(W)*H);
 for(UINT y=0;y<H;++y)memcpy(pixels.data()+size_t(y)*W,static_cast<char*>(m.pData)+size_t(y)*m.RowPitch,W*4);
 s.ctx->Unmap(dst.Get(),0);return pixels;
}
int wmain(int argc,wchar_t** argv){
 setvbuf(stdout,nullptr,_IONBF,0);puts("FG native DX11 NGX Latewarp probe | standalone synthetic image | no game access, no D3D12 or Streamline");
 try{
  auto folder=std::filesystem::absolute(argc>1?std::filesystem::path(argv[1]):std::filesystem::path(L"FrameWarpRuntime"));
  if(!std::filesystem::is_regular_file(folder/L"nvngx_latewarp.dll"))throw std::runtime_error("nvngx_latewarp.dll not found; provide original FrameWarpRuntime directory");
  ComPtr<IDXGIFactory1> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");ComPtr<IDXGIAdapter1> gpu;
  for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;auto r=factory->EnumAdapters1(i,&a);if(r==DXGI_ERROR_NOT_FOUND)break;hr(r,"Enumerate adapter");DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10DE&&!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){gpu=a;wprintf(L"Adapter: %ls\n",d.Description);break;}}
  if(!gpu)throw std::runtime_error("NVIDIA adapter not found");
  Session s;D3D_FEATURE_LEVEL actual{};hr(D3D11CreateDevice(gpu.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&s.dev,&actual,&s.ctx),"Native D3D11CreateDevice");
  auto logs=std::filesystem::temp_directory_path()/L"FG-NGX-DX11-Probe";std::filesystem::create_directories(logs);
  const wchar_t* paths[]={folder.c_str()};NVSDK_NGX_FeatureCommonInfo common{};common.PathListInfo.Path=paths;common.PathListInfo.Length=1;
  puts("STAGE 1: public NGX SDK native DX11 initialization");
  ngx(NVSDK_NGX_D3D11_Init_with_ProjectID("b6753093-8532-4a55-9109-4806f5c9b91a",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"FG probe 1.0",logs.c_str(),s.dev.Get(),&common,NVSDK_NGX_Version_API),"NGX DX11 init");s.init=true;
  ngx(NVSDK_NGX_D3D11_AllocateParameters(&s.p),"NGX DX11 allocate parameters");
  puts("STAGE 2: Latewarp feature 15 creation (no compatibility fallback)");
  s.p->Set("Latewarp.Output.Width",W);s.p->Set("Latewarp.Output.Height",H);
  ngx(NVSDK_NGX_D3D11_CreateFeature(s.ctx.Get(),NVSDK_NGX_Feature_Reserved15,s.p,&s.h),"NGX DX11 create Latewarp");
  if(!s.h)throw std::runtime_error("Null feature handle");
  puts("PASS: native DX11 Latewarp feature created. Starting synthetic evaluation.");
  std::vector<uint32_t> input(size_t(W)*H),zeros(input.size(),0);
  std::vector<float> depthPixels(input.size(),.999f);
  std::vector<uint8_t> maskPixels(input.size());
  for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){
   uint32_t v=((x/19+y/23)&1)?0xFF2BD0F0u:0xFFE04528u;
   input[size_t(y)*W+x]=v;
   const float dx=float(x)+.5f-W*.5f,dy=float(y)+.5f-H*.5f,d=std::sqrt(dx*dx+dy*dy);
   const float t=std::fmin(1.f,std::fmax(0.f,(d-40.f)/20.f));maskPixels[size_t(y)*W+x]=uint8_t(std::lround((1.f-t*t*(3.f-2.f*t))*255.f));
  }
  constexpr UINT sr=D3D11_BIND_SHADER_RESOURCE;
  auto color=texture(s.dev.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,sr,input.data(),W*4);
  auto ui=texture(s.dev.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,sr,zeros.data(),W*4);
  auto depth=texture(s.dev.Get(),DXGI_FORMAT_R32_FLOAT,sr,depthPixels.data(),W*4);
  auto motion=texture(s.dev.Get(),DXGI_FORMAT_R16G16_FLOAT,sr,zeros.data(),W*4);
  auto mask=texture(s.dev.Get(),DXGI_FORMAT_R8_UNORM,sr,maskPixels.data(),W);
  auto output=texture(s.dev.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,sr|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET,zeros.data(),W*4);
  auto* p=s.p;auto set=[&](const char* k,ID3D11Texture2D* r){p->Set(k,static_cast<ID3D11Resource*>(r));};
  set("Latewarp.Backbuffer",color.Get());set("Latewarp.HudlessColor",color.Get());set("Latewarp.UIColorAlpha",ui.Get());
  set("Depth",depth.Get());set("MotionVectors",motion.Get());set("Output",output.Get());set("Latewarp.NoWarpMask",mask.Get());
  for(const char* key:{"Latewarp.Backbuffer","Latewarp.HudlessColor","Latewarp.UIColorAlpha","Latewarp.Output","Latewarp.Depth","Latewarp.MV","Latewarp.NoWarpMask"}){
   const std::string k=key;p->Set((k+".Subrect.Base.X").c_str(),0u);p->Set((k+".Subrect.Base.Y").c_str(),0u);p->Set((k+".Subrect.Width").c_str(),W);p->Set((k+".Subrect.Height").c_str(),H);
  }
  std::array<float,16> source{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},target=source;
  const float f=1.3f,n=.1f,z=1000.f;
  std::array<float,16> proj{f/(float(W)/H),0,0,0,0,f,0,0,0,0,z/(z-n),1,0,0,-n*z/(z-n),0};
  p->Set("Latewarp.WorldToViewMatrix",static_cast<void*>(target.data()));p->Set("Latewarp.ViewToClipMatrix",static_cast<void*>(proj.data()));
  p->Set("Latewarp.PrevRenderedWorldToViewMatrix",static_cast<void*>(source.data()));p->Set("Latewarp.PrevRenderedViewToClipMatrix",static_cast<void*>(proj.data()));
  p->Set("Latewarp.DepthInverted",0u);p->Set("Latewarp.EvalFlags",0u);p->Set("Latewarp.UsePremultiplyUIAlpha",0u);
  puts("STAGE 3: native DX11 synthetic evaluation with center mask");
  p->Set("Latewarp.IsRenderedFrame",1u);p->Set("Latewarp.FrameID",1u);
  ngx(NVSDK_NGX_D3D11_EvaluateFeature(s.ctx.Get(),s.h,p,nullptr),"DX11 evaluate source");s.ctx->ClearState();auto a=read(s,output.Get());
  const float c=std::cos(.03f),q=std::sin(.03f);target={c,0,-q,0,0,1,0,0,q,0,c,0,0,0,0,1};
  p->Set("Latewarp.WorldToViewMatrix",static_cast<void*>(target.data()));p->Set("Latewarp.IsRenderedFrame",0u);p->Set("Latewarp.FrameID",2u);
  ngx(NVSDK_NGX_D3D11_EvaluateFeature(s.ctx.Get(),s.h,p,nullptr),"DX11 evaluate late pose");s.ctx->ClearState();auto b=read(s,output.Get());
  size_t changed=0,inside=0,outside=0;for(size_t i=0;i<a.size();++i)if(a[i]!=b[i]){++changed;if(maskPixels[i]==255)++inside;if(maskPixels[i]==0)++outside;}
  printf("Changed pixels: %zu | fully protected center changed: %zu | outside mask changed: %zu\n",changed,inside,outside);
  if(!outside){puts("INCONCLUSIVE: API succeeded, but synthetic warp was not observed.");return 3;}
  puts("PASS: native DX11 evaluation visibly changes output. Center mask behavior requires separate visual validation.");return 0;
 }catch(const std::exception& e){printf("STOP: %s\n",e.what());return 1;}
}
