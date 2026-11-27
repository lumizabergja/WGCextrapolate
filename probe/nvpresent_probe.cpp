#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <nvapi.h>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
static bool quit=false;
static LRESULT CALLBACK Wnd(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_CLOSE||(m==WM_KEYDOWN&&w==VK_ESCAPE)){quit=true;return 0;}
 return DefWindowProcW(h,m,w,l);
}
static void ck(HRESULT h,const char* what){if(FAILED(h)){char b[128];sprintf_s(b,"%s: 0x%08X",what,unsigned(h));throw std::runtime_error(b);}}
static LARGE_INTEGER freq{};
static double now(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return 1000.*double(t.QuadPart)/double(freq.QuadPart);}
static double quant(std::vector<double> a,double q){if(a.empty())return 0;std::sort(a.begin(),a.end());return a[size_t(q*(a.size()-1))];}
static void summary(const char* name,const std::vector<double>& a){printf("%s: n=%zu p50=%.3f ms p95=%.3f ms p99=%.3f ms\n",name,a.size(),quant(a,.5),quant(a,.95),quant(a,.99));}
int main(int argc,char** argv){
 bool nv=argc>1&&std::string(argv[1])=="nvapi";
 int hz=argc>2?std::clamp(atoi(argv[2]),30,320):320;
 QueryPerformanceFrequency(&freq);SetProcessDPIAware();
 printf("FG NvPresent probe | %s | %d Hz target\n",nv?"NVAPI wrapper":"DXGI",hz);
 const auto ns=NvAPI_Initialize();printf("NvAPI_Initialize=%d\n",int(ns));
 if(nv&&ns!=NVAPI_OK)return 2;
 NvU32 driver=0;NvAPI_ShortString branch{};if(ns==NVAPI_OK&&NvAPI_SYS_GetDriverAndBranchVersion(&driver,branch)==NVAPI_OK)printf("Driver=%u branch=%s\n",driver,branch);
 WNDCLASSW wc{};wc.lpfnWndProc=Wnd;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"FGPresentProbe";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));RegisterClassW(&wc);
 HWND win=CreateWindowW(wc.lpszClassName,L"FG present probe - Esc to stop",WS_OVERLAPPEDWINDOW|WS_VISIBLE,80,80,960,540,nullptr,nullptr,wc.hInstance,nullptr);
 if(!win)return 3;
 HANDLE wait=nullptr;int result=0;
 try{
  ComPtr<IDXGIFactory2> factory;ck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create factory");
  ComPtr<IDXGIAdapter1> adapter;
  for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;HRESULT e=factory->EnumAdapters1(i,&a);if(e==DXGI_ERROR_NOT_FOUND)break;ck(e,"Enumerate adapter");DXGI_ADAPTER_DESC1 d{};if(SUCCEEDED(a->GetDesc1(&d))&&d.VendorId==0x10de){adapter=a;break;}}
  if(!adapter)throw std::runtime_error("NVIDIA adapter not found");
  DXGI_ADAPTER_DESC1 ad{};adapter->GetDesc1(&ad);printf("Adapter vendor=%04X device=%04X\n",ad.VendorId,ad.DeviceId);
  ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;ck(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx),"Create D3D11 device");
  BOOL tearing=FALSE;ComPtr<IDXGIFactory5> f5;if(SUCCEEDED(factory.As(&f5)))f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing));
  RECT rect{};GetClientRect(win,&rect);
  DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=rect.right;sd.Height=rect.bottom;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT|(tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0);
  ComPtr<IDXGISwapChain1> swap;ck(factory->CreateSwapChainForHwnd(dev.Get(),win,&sd,nullptr,nullptr,&swap),"Create swapchain");factory->MakeWindowAssociation(win,DXGI_MWA_NO_ALT_ENTER);
  ComPtr<IDXGISwapChain2> s2;ck(swap.As(&s2),"Swapchain2");ck(s2->SetMaximumFrameLatency(1),"Frame latency 1");wait=s2->GetFrameLatencyWaitableObject();if(!wait)throw std::runtime_error("No frame latency waitable object");
  ComPtr<ID3D11Texture2D> buffer;ck(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Backbuffer");ComPtr<ID3D11RenderTargetView> rtv;ck(dev->CreateRenderTargetView(buffer.Get(),nullptr,&rtv),"RTV");
  printf("Tearing=%d | queue depth=1\n",tearing);
  auto module=[](){wchar_t path[32768]{};auto mod=GetModuleHandleW(L"NvPresent64.dll");if(mod){GetModuleFileNameW(mod,path,32768);printf("NvPresent64.dll loaded: %ls\n",path);}else printf("NvPresent64.dll not observed in probe process\n");};
  module();
  double start=now(),next=start,last=0;unsigned n=0,statsOk=0,statsFail=0,timeout=0;UINT firstCount=0,lastCount=0;
  std::vector<double> call,slot,cadence;
  while(!quit&&now()-start<12000.){
   MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}if(quit)break;
   double t=now();if(t<next){if(next-t>1.5)Sleep(1);else SwitchToThread();continue;}
   double w0=now();DWORD wr=WaitForSingleObject(wait,100);double w1=now();
   if(wr==WAIT_TIMEOUT){++timeout;continue;}if(wr!=WAIT_OBJECT_0)throw std::runtime_error("Waitable swapchain failed");
   bool warm=w1-start>=2000.;
   float v=float(.5+.5*std::sin(n*.08));float col[4]={v,.15f,1.f-v,1.f};ctx->ClearRenderTargetView(rtv.Get(),col);
   const UINT flags=tearing?DXGI_PRESENT_ALLOW_TEARING:0;double a=now();
   if(nv){auto st=NvAPI_D3D1x_Present(dev.Get(),swap.Get(),0,flags);if(st!=NVAPI_OK){printf("NvAPI present rejected: %d\n",int(st));throw std::runtime_error("NVAPI path not supported in this configuration");}}
   else ck(swap->Present(0,flags),"DXGI present");
   double b=now();++n;
   if(warm){call.push_back(b-a);slot.push_back(w1-w0);if(last)cadence.push_back(a-last);last=a;}
   if(warm&&n%16==0){DXGI_FRAME_STATISTICS st{};if(SUCCEEDED(swap->GetFrameStatistics(&st))){if(!statsOk)firstCount=st.PresentCount;lastCount=st.PresentCount;++statsOk;}else ++statsFail;}
   next+=1000./hz;if(next<b-1000./hz)next=b;
  }
  module();summary("CPU Present call",call);summary("CPU queue slot wait",slot);summary("CPU submission cadence",cadence);
  double elapsed=now()-start-2000.;printf("Measured submissions=%zu rate=%.1f/s timeouts=%u stats OK=%u failed=%u present count delta=%u\n",call.size(),elapsed>0?call.size()*1000./elapsed:0,timeout,statsOk,statsFail,statsOk?lastCount-firstCount:0);
  printf("CPU timing and DXGI statistics only; not actual scanout, generated FPS or mouse-to-photon latency.\n");
 }catch(const std::exception& e){printf("ERROR: %s\n",e.what());result=4;}
 if(wait)CloseHandle(wait);DestroyWindow(win);if(ns==NVAPI_OK)NvAPI_Unload();return result;
}
