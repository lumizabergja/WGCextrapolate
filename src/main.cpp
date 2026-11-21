// Fresh implementation based only on Microsoft WGC and NVIDIA OFAPI documentation.
// WGC -> persistent D3D11 NvOFA inputs -> FAST 4x4 flow -> edge-aware extrapolation -> flip discard.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <nvOpticalFlowD3D11.h>
#include <array>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <memory>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <cstring>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
using Microsoft::WRL::ComPtr;
namespace capture = winrt::Windows::Graphics::Capture;
namespace gfx = winrt::Windows::Graphics::DirectX;
using Clock = LARGE_INTEGER;

namespace {
constexpr UINT kMsgStatus = WM_APP+21;
constexpr UINT kHotkey = 0x7001;
constexpr UINT kIdWindow = 1001, kIdRefresh = 1002, kIdMode = 1003, kIdStart = 1004, kIdStop = 1005, kIdFullscreen=1006;
HWND gUi{},gWindowCombo{},gModeCombo{},gStart{},gStop{},gFull{};
std::atomic<bool> gRunning{false},gStopRequested{false};
std::thread gThread;
std::mutex gStatusMutex;
std::wstring gStatus=L"Select a window and press Start.";
struct Target {HWND hwnd;DWORD pid;std::wstring title;};
std::vector<Target> gTargets;
std::atomic<int> gMode{2}; // 1 off, 2=x2, 3=x3, 4=x4
std::atomic<bool> gFullScreen{true};

void Check(HRESULT hr,const char* context) { if(FAILED(hr)) {char s[200]; std::snprintf(s,sizeof(s),"%s (HRESULT 0x%08X)",context,(unsigned)hr);throw std::runtime_error(s);} }
void OfCheck(NV_OF_STATUS st,const char* context) {if(st!=NV_OF_SUCCESS){char s[180];std::snprintf(s,sizeof(s),"%s (NvOFA code %d)",context,(int)st);throw std::runtime_error(s);}}
std::wstring ToWide(const std::string& s) {int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);if(n<=0)return L"Unknown error";std::wstring w(size_t(n),L'\0');MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,w.data(),n);w.resize(size_t(n-1));return w;}
void Report(const std::wstring& s) { {std::lock_guard<std::mutex> lock(gStatusMutex);gStatus=s;} if(gUi) PostMessageW(gUi,kMsgStatus,0,0); }
LONGLONG Ticks(){LARGE_INTEGER x{};QueryPerformanceCounter(&x);return x.QuadPart;}
LONGLONG Frequency(){LARGE_INTEGER x{};QueryPerformanceFrequency(&x);return x.QuadPart;}
BOOL CALLBACK EnumTarget(HWND h,LPARAM){if(!IsWindowVisible(h)||h==gUi||GetWindow(h,GW_OWNER))return TRUE; wchar_t name[256]{};GetWindowTextW(h,name,256);if(!name[0])return TRUE; RECT r{};GetWindowRect(h,&r);if(r.right-r.left<320||r.bottom-r.top<200)return TRUE;DWORD pid{};GetWindowThreadProcessId(h,&pid);if(!pid||pid==GetCurrentProcessId())return TRUE;gTargets.push_back({h,pid,name});return TRUE;}
void Refresh(){SendMessageW(gWindowCombo,CB_RESETCONTENT,0,0);gTargets.clear();EnumWindows(EnumTarget,0);for(auto const& t:gTargets)SendMessageW(gWindowCombo,CB_ADDSTRING,0,(LPARAM)t.title.c_str());if(!gTargets.empty())SendMessageW(gWindowCombo,CB_SETCURSEL,0,0);}

// The flow is signed S10.5 (divide by 32 to get pixels). NvOFA vectors are PREVIOUS -> CURRENT.
// Future frame at t is a backward gather from the latest frame at position p - t * flow.
// Edge-aware interpolation uses four grid vectors and luma-guided weighting.
// Occlusion fallback compares previous warped appearance against current pixel to reject unreliable flow.
static const char kShader[] = R"HLSL(
cbuffer Params:register(b0) { float2 dimensions; float2 flowDims; float extrapolate; float hasFlow; float2 pad0; };
Texture2D<float4> latestTex:register(t0);
Texture2D<float4> previousTex:register(t1);
Texture2D<int2> vectors:register(t2);
SamplerState smoothSample:register(s0);
struct VSOut{float4 pos:SV_Position;float2 uv:TEXCOORD0;};
VSOut VSMain(uint vertex:SV_VertexID){
 VSOut o; float2 p=float2((vertex<<1)&2, vertex&2);
 o.pos=float4(p*float2(2,-2)+float2(-1,1),0,1);o.uv=p;return o;
}
float Luma(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
float2 ReadFlow(int2 g){g=clamp(g,int2(0,0),int2(flowDims)-1);return float2(vectors.Load(int3(g,0)))/32.0;}
float2 EdgeAwareFlow(float2 pixel, float centerLuma){
 float2 grid=pixel*0.25-0.5;int2 base=(int2)floor(grid);float2 fr=frac(grid);
 float2 sum=float2(0,0);float total=0;
 [unroll]for(int y=0;y<2;y++){[unroll]for(int x=0;x<2;x++){
  int2 gp=clamp(base+int2(x,y),int2(0,0),int2(flowDims)-1);
  float2 blockPos=(float2(gp)+0.5)*4.0;
  float l=Luma(latestTex.SampleLevel(smoothSample,saturate(blockPos/dimensions),0).rgb);
  float w=(x==0?1-fr.x:fr.x)*(y==0?1-fr.y:fr.y);
  w*=rcp(1.0+abs(l-centerLuma)*12.0);
  sum+=ReadFlow(gp)*w;total+=w;
 }}return sum/max(total,1e-5);
}
float4 PSMain(VSOut i):SV_Target{
 float2 uv=saturate(i.uv);float4 current=latestTex.SampleLevel(smoothSample,uv,0);
 if(hasFlow<0.5 || extrapolate<0.001) return float4(current.rgb,1.0);
 float2 pixels=uv*dimensions;
 float2 vec=EdgeAwareFlow(pixels,Luma(current.rgb));
 // A second fixed-point step approximates flow defined at the backward-warped source.
 float2 firstUv=saturate(uv-vec*(extrapolate/dimensions));
 float2 vec2=EdgeAwareFlow(firstUv*dimensions,Luma(latestTex.SampleLevel(smoothSample,firstUv,0).rgb));
 vec=lerp(vec,vec2,0.35);
 vec=clamp(vec,float2(-256,-256),float2(256,256));
 float2 futureUv=saturate(uv-vec*(extrapolate/dimensions));
 float4 predicted=latestTex.SampleLevel(smoothSample,futureUv,0);
 float2 priorUv=saturate(uv-vec/dimensions);
 float3 prev=previousTex.SampleLevel(smoothSample,priorUv,0).rgb;
 // Reject cross-object vectors / uncovered regions without a hardware cost map.
 float mismatch=abs(Luma(prev)-Luma(current.rgb));
 float trust=saturate((0.20-mismatch)*8.0);
 // Avoid drawing black borders from exaggerated motion near the boundaries.
 float2 edge=min(futureUv,1.0-futureUv)*dimensions;
 trust*=saturate(min(edge.x,edge.y)*0.5);
 return float4(lerp(current.rgb,predicted.rgb,trust),1.0);
}
)HLSL";

struct D3D {
 ComPtr<ID3D11Device> dev;
 ComPtr<ID3D11DeviceContext> ctx;
 ComPtr<IDXGISwapChain1> swap;
 ComPtr<ID3D11VertexShader> vs;
 ComPtr<ID3D11PixelShader> ps;
 ComPtr<ID3D11SamplerState> sampler;
 ComPtr<ID3D11Buffer> constants;
 ComPtr<ID3D11RenderTargetView> rtv;
 ComPtr<IDXGIDevice1> dxgi;
 UINT sourceW{},sourceH{},outputW{},outputH{};
 bool tearing{};
 struct CB {float dimensions[2],flowDims[2],extrapolate,hasFlow,pad[2];};
 explicit D3D(HWND presenter, UINT w,UINT h,UINT outW,UINT outH) : sourceW(w),sourceH(h),outputW(outW),outputH(outH) {
  UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL obtained{};
  Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&dev,&obtained,&ctx),"D3D11CreateDevice");
  ComPtr<ID3D11Multithread> multi;Check(ctx.As(&multi),"Get ID3D11Multithread");multi->SetMultithreadProtected(TRUE);
  Check(dev.As(&dxgi),"Get IDXGIDevice1");dxgi->SetMaximumFrameLatency(1);
  ComPtr<IDXGIAdapter> adapter;Check(dxgi->GetAdapter(&adapter),"DXGI GetAdapter");
  ComPtr<IDXGIFactory2> factory;Check(adapter->GetParent(IID_PPV_ARGS(&factory)),"DXGI GetFactory");
  ComPtr<IDXGIFactory5> factory5;
  if(SUCCEEDED(factory.As(&factory5))) {BOOL supported=FALSE;if(SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof(supported))))tearing=!!supported;}
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width=outW;desc.Height=outH;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferCount=2;
  desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.Scaling=DXGI_SCALING_STRETCH;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
  desc.Flags=tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
  Check(factory->CreateSwapChainForHwnd(dev.Get(),presenter,&desc,nullptr,nullptr,&swap),"CreateSwapChainForHwnd");
  factory->MakeWindowAssociation(presenter,DXGI_MWA_NO_ALT_ENTER);
  ID3DBlob* vsBlob=nullptr;ID3DBlob* psBlob=nullptr;ID3DBlob* diagnostics=nullptr;
  HRESULT hr=D3DCompile(kShader,strlen(kShader),"embedded.hlsl",nullptr,nullptr,"VSMain","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vsBlob,&diagnostics);
  if(FAILED(hr)) {std::string s="VS shader compile: ";if(diagnostics)s.append((char*)diagnostics->GetBufferPointer(),diagnostics->GetBufferSize());if(diagnostics)diagnostics->Release();throw std::runtime_error(s);}
  if(diagnostics){diagnostics->Release();diagnostics=nullptr;}
  ComPtr<ID3DBlob> vertex;vertex.Attach(vsBlob);
  Check(dev->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs),"CreateVertexShader");
  hr=D3DCompile(kShader,strlen(kShader),"embedded.hlsl",nullptr,nullptr,"PSMain","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&psBlob,&diagnostics);
  if(FAILED(hr)){std::string s="PS shader compile: ";if(diagnostics)s.append((char*)diagnostics->GetBufferPointer(),diagnostics->GetBufferSize());if(diagnostics)diagnostics->Release();throw std::runtime_error(s);}
  if(diagnostics)diagnostics->Release();
  ComPtr<ID3DBlob> pixel;pixel.Attach(psBlob);
  Check(dev->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&ps),"CreatePixelShader");
  D3D11_SAMPLER_DESC smp{};smp.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;smp.AddressU=smp.AddressV=smp.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;smp.MaxLOD=D3D11_FLOAT32_MAX;
  Check(dev->CreateSamplerState(&smp,&sampler),"CreateSamplerState");
  D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(CB);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
  Check(dev->CreateBuffer(&bd,nullptr,&constants),"CreateConstantBuffer");
 }

 void Blit(ID3D11Texture2D* dest, ID3D11ShaderResourceView* src){
  // Only used when NVIDIA advertises RGBA input but not BGRA. 100% GPU-resident.
  ComPtr<ID3D11RenderTargetView> destRTV;
  Check(dev->CreateRenderTargetView(dest,nullptr,&destRTV),"Create RGBA conversion RTV");
  D3D11_VIEWPORT vp{};vp.Width=float(sourceW);vp.Height=float(sourceH);vp.MinDepth=0;vp.MaxDepth=1;
  ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargets(1,destRTV.GetAddressOf(),nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
  ctx->PSSetSamplers(0,1,sampler.GetAddressOf());
  CB cb{};cb.dimensions[0]=float(sourceW);cb.dimensions[1]=float(sourceH);
  cb.flowDims[0]=float((sourceW+3)/4);cb.flowDims[1]=float((sourceH+3)/4);
  ctx->UpdateSubresource(constants.Get(),0,nullptr,&cb,0,0);
  ctx->PSSetConstantBuffers(0,1,constants.GetAddressOf());
  ID3D11ShaderResourceView* views[3]={src,nullptr,nullptr};ctx->PSSetShaderResources(0,3,views);
  ctx->Draw(3,0);
  ID3D11ShaderResourceView* none[3]={nullptr,nullptr,nullptr};ctx->PSSetShaderResources(0,3,none);
  ID3D11RenderTargetView* nil=nullptr;ctx->OMSetRenderTargets(1,&nil,nullptr);
 }

 void Present(ID3D11ShaderResourceView* latest,ID3D11ShaderResourceView* previous,ID3D11ShaderResourceView* flow,float extrapolate,bool flowValid){
  ComPtr<ID3D11Texture2D> back;Check(swap->GetBuffer(0,IID_PPV_ARGS(&back)),"SwapChain GetBuffer");
  rtv.Reset();Check(dev->CreateRenderTargetView(back.Get(),nullptr,&rtv),"CreateRenderTargetView");
  D3D11_VIEWPORT vp{};vp.Width=(float)outputW;vp.Height=(float)outputH;vp.MinDepth=0;vp.MaxDepth=1;
  ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
  ctx->PSSetSamplers(0,1,sampler.GetAddressOf());
  CB cb{};cb.dimensions[0]=(float)sourceW;cb.dimensions[1]=(float)sourceH;
  cb.flowDims[0]=(float)((sourceW+3)/4);cb.flowDims[1]=(float)((sourceH+3)/4);
  cb.extrapolate=extrapolate;cb.hasFlow=flowValid?1.0f:0.0f;
  ctx->UpdateSubresource(constants.Get(),0,nullptr,&cb,0,0);
  ctx->PSSetConstantBuffers(0,1,constants.GetAddressOf());
  ID3D11ShaderResourceView* views[3]={latest,previous?previous:latest,flow};ctx->PSSetShaderResources(0,3,views);
  ctx->Draw(3,0);
  ID3D11ShaderResourceView* nullViews[3]={nullptr,nullptr,nullptr};ctx->PSSetShaderResources(0,3,nullViews);
  ID3D11RenderTargetView* nullRTV=nullptr;ctx->OMSetRenderTargets(1,&nullRTV,nullptr);
  // Present(0) prioritizes low latency and may tear. Output frequency is still scheduled externally.
  HRESULT hr=swap->Present(0,tearing?DXGI_PRESENT_ALLOW_TEARING:0);
  Check(hr,"Present");
  rtv.Reset();
 }
};

struct NvFlow {
 HMODULE module{};
 NV_OF_D3D11_API_FUNCTION_LIST api{};
 NvOFHandle handle{};
 std::array<ComPtr<ID3D11Texture2D>,3> frames;
 std::array<ComPtr<ID3D11ShaderResourceView>,3> frameViews;
 std::array<NvOFGPUBufferHandle,3> frameHandles{};
 std::array<ComPtr<ID3D11Texture2D>,3> outputs;
 std::array<ComPtr<ID3D11ShaderResourceView>,3> outputViews;
 std::array<NvOFGPUBufferHandle,3> outputHandles{};
 DXGI_FORMAT inputFormat{DXGI_FORMAT_UNKNOWN};
 UINT width{},height{};
 D3D* d3d{};
 explicit NvFlow(D3D& d):width(d.sourceW),height(d.sourceH),d3d(&d){
  module=LoadLibraryW(L"nvofapi64.dll");if(!module)throw std::runtime_error("nvofapi64.dll was not found (NVIDIA driver Optical Flow component)");
  auto create=reinterpret_cast<NV_OF_STATUS(NVOFAPI*)(uint32_t,NV_OF_D3D11_API_FUNCTION_LIST*)>(GetProcAddress(module,"NvOFAPICreateInstanceD3D11"));
  if(!create)throw std::runtime_error("NvOFAPICreateInstanceD3D11 export is missing");
  OfCheck(create(NV_OF_API_VERSION,&api),"NvOFAPICreateInstanceD3D11");
  OfCheck(api.nvCreateOpticalFlowD3D11(d.dev.Get(),d.ctx.Get(),&handle),"nvCreateOpticalFlowD3D11");
  NV_OF_INIT_PARAMS init{};init.width=width;init.height=height;init.mode=NV_OF_MODE_OPTICALFLOW;
  init.perfLevel=NV_OF_PERF_LEVEL_FAST;init.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
  init.enableOutputCost=NV_OF_FALSE;init.enableExternalHints=NV_OF_FALSE;
  init.predDirection=NV_OF_PRED_DIRECTION_FORWARD;init.enableGlobalFlow=NV_OF_FALSE;
  init.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;
  // Initialize before enumerating supported NvOFA resource formats.
  OfCheck(api.nvOFInit(handle,&init),"NvOFInit FAST 4x4 cost OFF");
  auto fmts=Formats(NV_OF_BUFFER_USAGE_INPUT);
  for(DXGI_FORMAT f:fmts)if(f==DXGI_FORMAT_B8G8R8A8_UNORM){inputFormat=f;break;}
  // RGBA is used only when necessary; it requires one additional GPU blit.
  if(inputFormat==DXGI_FORMAT_UNKNOWN){for(DXGI_FORMAT f:fmts)if(f==DXGI_FORMAT_R8G8B8A8_UNORM){inputFormat=f;break;}}
  if(inputFormat==DXGI_FORMAT_UNKNOWN)throw std::runtime_error("NvOFA input BGRA8/RGBA8 formats not advertised; no compatible GPU path");
  auto outputFmts=Formats(NV_OF_BUFFER_USAGE_OUTPUT);
  if(std::find(outputFmts.begin(),outputFmts.end(),DXGI_FORMAT_R16G16_SINT)==outputFmts.end())throw std::runtime_error("NvOFA does not advertise R16G16_SINT optical-flow output");
  D3D11_TEXTURE2D_DESC in{};in.Width=width;in.Height=height;in.MipLevels=1;in.ArraySize=1;
  in.Format=inputFormat;in.SampleDesc.Count=1;in.Usage=D3D11_USAGE_DEFAULT;in.BindFlags=D3D11_BIND_SHADER_RESOURCE|(inputFormat==DXGI_FORMAT_B8G8R8A8_UNORM?0:D3D11_BIND_RENDER_TARGET);
  for(int i=0;i<3;i++){
   Check(d.dev->CreateTexture2D(&in,nullptr,&frames[i]),"Create NvOFA input texture");
   Check(d.dev->CreateShaderResourceView(frames[i].Get(),nullptr,&frameViews[i]),"Create input SRV");
   OfCheck(api.nvOFRegisterResourceD3D11(handle,frames[i].Get(),&frameHandles[i]),"Register NvOFA input");
  }
  D3D11_TEXTURE2D_DESC out=in;out.Width=(width+3)/4;out.Height=(height+3)/4;
  out.Format=DXGI_FORMAT_R16G16_SINT;out.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
  for(int i=0;i<3;i++){
   Check(d.dev->CreateTexture2D(&out,nullptr,&outputs[i]),"Create NvOFA output texture");
   Check(d.dev->CreateShaderResourceView(outputs[i].Get(),nullptr,&outputViews[i]),"Create NvOFA output SRV");
   OfCheck(api.nvOFRegisterResourceD3D11(handle,outputs[i].Get(),&outputHandles[i]),"Register NvOFA output");
  }
 }
 std::vector<DXGI_FORMAT> Formats(NV_OF_BUFFER_USAGE usage){
  uint32_t count=0;OfCheck(api.nvOFGetSurfaceFormatCountD3D11(handle,usage,NV_OF_MODE_OPTICALFLOW,&count),"NvOFA format count");
  if(count==0||count>256)throw std::runtime_error("NvOFA returned invalid format count");
  std::vector<DXGI_FORMAT> formats(count);
  OfCheck(api.nvOFGetSurfaceFormatD3D11(handle,usage,NV_OF_MODE_OPTICALFLOW,formats.data()),"NvOFA formats");
  return formats;
 }
 void Execute(int earlier,int later,int output){
  NV_OF_EXECUTE_INPUT_PARAMS in{};in.inputFrame=frameHandles[earlier];in.referenceFrame=frameHandles[later];
  in.disableTemporalHints=NV_OF_FALSE;
  NV_OF_EXECUTE_OUTPUT_PARAMS out{};out.outputBuffer=outputHandles[output];
  OfCheck(api.nvOFExecute(handle,&in,&out),"NvOFExecute");
 }
 ~NvFlow(){
  if(handle){for(auto& f:frameHandles)if(f)api.nvOFUnregisterResourceD3D11(f);
   for(auto& f:outputHandles)if(f)api.nvOFUnregisterResourceD3D11(f);
   api.nvOFDestroy(handle);}
  if(module)FreeLibrary(module);
 }
};

LRESULT CALLBACK OutputWndProc(HWND h,UINT msg,WPARAM w,LPARAM l){
 if(msg==WM_NCHITTEST)return HTTRANSPARENT; // May not guarantee click-through in every Windows mode.
 if(msg==WM_CLOSE){gStopRequested=true;return 0;}
 return DefWindowProcW(h,msg,w,l);
}
void Engine(Target target){
 HWND view=nullptr;
 try {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  RECT rect{};if(!GetWindowRect(target.hwnd,&rect))throw std::runtime_error("Selected window no longer exists");
  const HMONITOR monitor=MonitorFromWindow(target.hwnd,MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(monitor,&mi);
  const bool fullscreen=gFullScreen.load();
  UINT outW=(UINT)(mi.rcMonitor.right-mi.rcMonitor.left),outH=(UINT)(mi.rcMonitor.bottom-mi.rcMonitor.top);
  if(!fullscreen){outW=960;outH=540;}
  WNDCLASSW wc{};wc.lpfnWndProc=OutputWndProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"WgcNewExtrapolatorOutput";
  RegisterClassW(&wc);
  const int left=fullscreen?mi.rcMonitor.left:mi.rcMonitor.left+40,top=fullscreen?mi.rcMonitor.top:mi.rcMonitor.top+40;
  view=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,wc.lpszClassName,L"WGC New Extrapolation",
    WS_POPUP,left,top,(int)outW,(int)outH,nullptr,nullptr,wc.hInstance,nullptr);
  if(!view)throw std::runtime_error("Create output window failed");
  ShowWindow(view,SW_SHOWNOACTIVATE);SetWindowPos(view,HWND_TOPMOST,left,top,(int)outW,(int)outH,SWP_NOACTIVATE);
  auto factory=winrt::get_activation_factory<capture::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
  capture::GraphicsCaptureItem item{nullptr};
  Check(factory->CreateForWindow(target.hwnd,winrt::guid_of<capture::GraphicsCaptureItem>(),winrt::put_abi(item)),"GraphicsCaptureItem from HWND");
  auto size=item.Size();if(size.Width<=0||size.Height<=0)throw std::runtime_error("Selected window has invalid capture dimensions");
  D3D d3d(view,(UINT)size.Width,(UINT)size.Height,outW,outH);
  // Use the same Direct3D device as WGC and NvOFA; no cross-device texture transfer.
  winrt::com_ptr<::IInspectable> inspect;
  Check(CreateDirect3D11DeviceFromDXGIDevice(d3d.dxgi.Get(),inspect.put()),"Create WinRT D3D11 device");
  auto winDevice=inspect.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  auto pool=capture::Direct3D11CaptureFramePool::CreateFreeThreaded(winDevice,gfx::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
  auto session=pool.CreateCaptureSession(item);
  if(auto ver=session.try_as<capture::IGraphicsCaptureSession2>())ver.IsCursorCaptureEnabled(false);
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("CreateEvent failed");
  auto token=pool.FrameArrived([event](auto const&,auto const&){SetEvent(event);});
  std::unique_ptr<NvFlow> optical;
  // NvOFA only runs when mode is x2-x4; Off still runs without an NvOFA session.
  int current=-1,prior=-1,nextSlot=0,outSlot=0,pending=0;bool validFlow=false;
  LONGLONG freq=Frequency(),lastReal=0,baseInterval=freq/60,nextGen=0;
  size_t realCount=0,genCount=0;
  session.StartCapture();Report(L"Running: WGC native D3D11 capture");
  while(!gStopRequested.load() && IsWindow(target.hwnd)){
   // Drain the pool to newest unique available source frame, never queue stale images.
   capture::Direct3D11CaptureFrame newest{nullptr};
   for(int n=0;n<8;n++){auto f=pool.TryGetNextFrame();if(!f)break;if(newest)newest.Close();newest=std::move(f);}
   if(newest){
    auto content=newest.ContentSize();
    if(content.Width!=(int)d3d.sourceW||content.Height!=(int)d3d.sourceH)throw std::runtime_error("Capture resolution changed; stop and restart to reinitialize NvOFA");
    auto access=newest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> captureTex;
    Check(access->GetInterface(IID_PPV_ARGS(&captureTex)),"Get WGC D3D11 texture");
    const int mode=gMode.load();
    if(mode>1&&!optical)optical=std::make_unique<NvFlow>(d3d);
    if(mode==1 && !optical)optical=std::make_unique<NvFlow>(d3d); // same texture storage for live Off switching
    if(lastReal){LONGLONG age=Ticks()-lastReal;if(age>freq/500 && age<freq/15)baseInterval=(baseInterval*3+age)/4;}
    const int slot=nextSlot;nextSlot=(nextSlot+1)%3;
    if(optical->inputFormat==DXGI_FORMAT_B8G8R8A8_UNORM){
      d3d.ctx->CopyResource(optical->frames[slot].Get(),captureTex.Get()); // one GPU copy, no CPU readback
    }else{
      ComPtr<ID3D11ShaderResourceView> sourceView;
      Check(d3d.dev->CreateShaderResourceView(captureTex.Get(),nullptr,&sourceView),"Create WGC source SRV for RGBA conversion");
      d3d.Blit(optical->frames[slot].Get(),sourceView.Get()); // necessary GPU format conversion
    }
    newest.Close();
    prior=current;current=slot;lastReal=Ticks();realCount++;
    validFlow=false;
    // Real source frame always submits immediately before optical flow or generated frames.
    d3d.Present(optical->frameViews[current].Get(),nullptr,nullptr,0,false);
    if(prior>=0 && mode>1){
      outSlot=(outSlot+1)%3;optical->Execute(prior,current,outSlot);validFlow=true;
      pending=1;nextGen=lastReal+baseInterval/mode;
    }else pending=0;
    continue;
   }
   int mode=gMode.load();
   if(mode<=1||!validFlow||pending<=0||pending>=mode){
    WaitForSingleObject(event,2);continue;
   }
   const LONGLONG now=Ticks();
   if(now<nextGen){const LONGLONG remain=nextGen-now;
    if(remain>freq/700){DWORD ms=(DWORD)std::min<LONGLONG>(20,(remain*1000)/freq-1);WaitForSingleObject(event,ms?ms:1);}
    else if(remain>freq/20000)SwitchToThread();
    continue;
   }
   // Do not display predicted frames beyond one source period: stale visuals create a laggy feeling.
   if(now-lastReal>baseInterval || now-lastReal>freq/15){pending=0;continue;}
   const float alpha=float(pending)/float(mode);
   d3d.Present(optical->frameViews[current].Get(),optical->frameViews[prior].Get(),optical->outputViews[outSlot].Get(),alpha,true);
   genCount++;pending++;nextGen=lastReal+baseInterval*pending/mode;
   if(pending>=mode)pending=0;
   if((realCount+genCount)%500==0){wchar_t s[200];swprintf_s(s,L"WGC real: %zu  generated: %zu  NvOFA FAST 4x4 / cost OFF",realCount,genCount);Report(s);}
  }
  pool.FrameArrived(token);session.Close();pool.Close();CloseHandle(event);
 }catch(std::exception const& ex){Report(L"Error: "+ToWide(ex.what()));}
 if(view)DestroyWindow(view);
 gRunning=false;PostMessageW(gUi,WM_APP+22,0,0);
}

LRESULT CALLBACK UiWndProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
 switch(msg){
 case WM_CREATE:{
   CreateWindowW(L"STATIC",L"Capture window",WS_CHILD|WS_VISIBLE,18,18,200,22,h,nullptr,nullptr,nullptr);
   gWindowCombo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,18,45,345,280,h,(HMENU)kIdWindow,nullptr,nullptr);
   CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE,370,44,85,28,h,(HMENU)kIdRefresh,nullptr,nullptr);
   CreateWindowW(L"STATIC",L"Extrapolation",WS_CHILD|WS_VISIBLE,18,96,200,20,h,nullptr,nullptr,nullptr);
   gModeCombo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,18,122,180,170,h,(HMENU)kIdMode,nullptr,nullptr);
   for(const wchar_t* t:{L"Off",L"x2",L"x3",L"x4"})SendMessageW(gModeCombo,CB_ADDSTRING,0,(LPARAM)t);
   SendMessageW(gModeCombo,CB_SETCURSEL,1,0);
   gFull=CreateWindowW(L"BUTTON",L"Fullscreen output",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,210,121,190,26,h,(HMENU)kIdFullscreen,nullptr,nullptr);
   SendMessageW(gFull,BM_SETCHECK,BST_CHECKED,0);
   gStart=CreateWindowW(L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE,18,168,120,36,h,(HMENU)kIdStart,nullptr,nullptr);
   gStop=CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE,150,168,120,36,h,(HMENU)kIdStop,nullptr,nullptr);
   CreateWindowW(L"STATIC",L"Ctrl + Alt + F8: emergency Stop",WS_CHILD|WS_VISIBLE,18,216,400,22,h,nullptr,nullptr,nullptr);
   CreateWindowW(L"STATIC",L"Low latency: WGC / D3D11 / NvOFA FAST 4x4",WS_CHILD|WS_VISIBLE,18,246,460,20,h,nullptr,nullptr,nullptr);
   Refresh();RegisterHotKey(h,kHotkey,MOD_CONTROL|MOD_ALT,VK_F8);return 0;
 }
 case WM_HOTKEY:if(wp==kHotkey){gStopRequested=true;return 0;}break;
 case WM_COMMAND:{const UINT id=LOWORD(wp);
   if(id==kIdRefresh){if(!gRunning)Refresh();return 0;}
   if(id==kIdMode&&HIWORD(wp)==CBN_SELCHANGE){gMode=1+(int)SendMessageW(gModeCombo,CB_GETCURSEL,0,0);return 0;}
   if(id==kIdStart&&!gRunning){int ix=(int)SendMessageW(gWindowCombo,CB_GETCURSEL,0,0);
    if(ix<0||ix>=(int)gTargets.size()){MessageBoxW(h,L"Select a window first.",L"WGC Extrapolation",MB_OK);return 0;}
    if(gThread.joinable())gThread.join();gStopRequested=false;gRunning=true;
    gMode=1+(int)SendMessageW(gModeCombo,CB_GETCURSEL,0,0);
    gFullScreen=(SendMessageW(gFull,BM_GETCHECK,0,0)==BST_CHECKED);
    Target t=gTargets[ix];gThread=std::thread([t]{Engine(t);});EnableWindow(gStart,FALSE);return 0;
   }
   if(id==kIdStop){gStopRequested=true;return 0;}
   break;
 }
 case kMsgStatus:{std::wstring s;{std::lock_guard<std::mutex> lock(gStatusMutex);s=gStatus;}SetWindowTextW(h,(L"WGC NvOFA Extrapolation - "+s).c_str());return 0;}
 case WM_APP+22:EnableWindow(gStart,TRUE);return 0;
 case WM_CLOSE:gStopRequested=true;if(gThread.joinable())gThread.join();DestroyWindow(h);return 0;
 case WM_DESTROY:UnregisterHotKey(h,kHotkey);gUi=nullptr;PostQuitMessage(0);return 0;
 }
 return DefWindowProcW(h,msg,wp,lp);
}
} // namespace
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,LPWSTR,int show){
 try{
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  WNDCLASSW wc{};wc.lpfnWndProc=UiWndProc;wc.hInstance=inst;wc.lpszClassName=L"FreshWGCNVOFAControl";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
  RegisterClassW(&wc);
  gUi=CreateWindowExW(0,wc.lpszClassName,L"WGC NvOFA Extrapolation - Fresh",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VISIBLE,
    CW_USEDEFAULT,CW_USEDEFAULT,488,330,nullptr,nullptr,inst,nullptr);
  if(!gUi)throw std::runtime_error("Failed to create main window");ShowWindow(gUi,show);
  MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}
  if(gThread.joinable())gThread.join();return (int)m.wParam;
 }catch(std::exception const& e){MessageBoxW(nullptr,ToWide(e.what()).c_str(),L"Startup failure",MB_OK|MB_ICONERROR);return 1;}
}
