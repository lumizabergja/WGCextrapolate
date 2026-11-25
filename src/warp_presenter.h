#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <climits>
namespace fgwarp {
using Microsoft::WRL::ComPtr;
inline std::atomic<bool> requested{false};
inline void (*stageCallback)(const wchar_t*){};
inline void stage(const wchar_t* message) noexcept {if(stageCallback)stageCallback(message);}
inline std::atomic<long long> mouseX{},mouseY{};
struct Handle {HANDLE value{};~Handle(){if(value)CloseHandle(value);}};
inline void check(HRESULT r,const char* name){if(FAILED(r)){char s[160];std::snprintf(s,sizeof(s),"%s (0x%08X)",name,unsigned(r));throw std::runtime_error(s);}}
inline void ngx(NVSDK_NGX_Result r,const char* name){if(!NVSDK_NGX_SUCCEED(r)){char s[160];std::snprintf(s,sizeof(s),"%s (NGX 0x%08X)",name,unsigned(r));throw std::runtime_error(s);}}
struct Presenter {
    bool initialized{},sdkReady{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> color,output,depth,motion,ui,mask;
    ComPtr<ID3D11RenderTargetView> inputRTV;
    ComPtr<ID3D11ShaderResourceView> outputSRV;
    NVSDK_NGX_Parameter* params{};
    NVSDK_NGX_Handle* feature{};
    UINT w{},h{},radiusCache{UINT_MAX},featherCache{UINT_MAX},frameID{};
    float aspect{},yaw{},pitch{};
    std::array<float,16> source{},target{},projection{};
    static constexpr double radiansPerCount=-.022*1.5*3.141592653589793/180.;
    static float angle(long long n){return float(std::remainder(n*radiansPerCount,6.283185307179586));}
    static std::array<float,16> camera(float y,float p){float cy=std::cos(y),sy=std::sin(y),cp=std::cos(p),sp=std::sin(p);return {cy,sy*sp,-sy*cp,0,0,cp,sp,0,sy,-cy*sp,cy*cp,0,0,0,0,1};}
    void sourcePose(){yaw=angle(mouseX.load());pitch=angle(mouseY.load());}
    ComPtr<ID3D11Texture2D> tex(DXGI_FORMAT fmt,UINT bind,const void* initial=nullptr,UINT pitchBytes=0){
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.Format=fmt;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA data{initial,pitchBytes,0};ComPtr<ID3D11Texture2D> out;check(device->CreateTexture2D(&d,initial?&data:nullptr,&out),"Native NGX DX11 texture");return out;
    }
    void open(ID3D11Device* dev,ID3D11DeviceContext* context,UINT width,UINT height,UINT srcW,UINT srcH){
        stage(L"Native DX11 NGX initialization");
        device=dev;ctx=context;w=width;h=height;aspect=float(srcW)/srcH;
        wchar_t exe[32768]{};if(!GetModuleFileNameW(nullptr,exe,32768))throw std::runtime_error("Find FG path");
        const auto folder=std::filesystem::path(exe).parent_path()/L"FrameWarpRuntime";
        if(!std::filesystem::is_regular_file(folder/L"nvngx_latewarp.dll"))throw std::runtime_error("Native NGX: original FrameWarpRuntime/nvngx_latewarp.dll missing");
        const auto logs=std::filesystem::temp_directory_path()/L"FG-NGX-DX11";std::filesystem::create_directories(logs);
        const wchar_t* paths[]={folder.c_str()};NVSDK_NGX_FeatureCommonInfo common{};common.PathListInfo.Path=paths;common.PathListInfo.Length=1;
        ngx(NVSDK_NGX_D3D11_Init_with_ProjectID("b6753093-8532-4a55-9109-4806f5c9b91a",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"FG 1.0",logs.c_str(),device.Get(),&common,NVSDK_NGX_Version_API),"Native NGX DX11 init");sdkReady=true;
        ngx(NVSDK_NGX_D3D11_AllocateParameters(&params),"Native NGX DX11 params");
        params->Set("Latewarp.Output.Width",w);params->Set("Latewarp.Output.Height",h);
        stage(L"Native DX11 NGX Latewarp feature creation");
        ngx(NVSDK_NGX_D3D11_CreateFeature(ctx.Get(),NVSDK_NGX_Feature_Reserved15,params,&feature),"Native NGX DX11 Latewarp create");
        if(!feature)throw std::runtime_error("Native NGX null feature");
        stage(L"Native DX11 NGX texture allocation");
        constexpr UINT sr=D3D11_BIND_SHADER_RESOURCE;
        color=tex(DXGI_FORMAT_R8G8B8A8_UNORM,sr|D3D11_BIND_RENDER_TARGET);
        output=tex(DXGI_FORMAT_R8G8B8A8_UNORM,sr|D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_RENDER_TARGET);
        std::vector<uint32_t> zeros(size_t(w)*h,0);
        std::vector<float> depths(zeros.size(),.999f);
        depth=tex(DXGI_FORMAT_R32_FLOAT,sr,depths.data(),w*4);
        motion=tex(DXGI_FORMAT_R16G16_FLOAT,sr,zeros.data(),w*4);
        ui=tex(DXGI_FORMAT_R8G8B8A8_UNORM,sr,zeros.data(),w*4);
        mask=tex(DXGI_FORMAT_R8_UNORM,sr);
        check(device->CreateRenderTargetView(color.Get(),nullptr,&inputRTV),"Native NGX input RTV");
        check(device->CreateShaderResourceView(output.Get(),nullptr,&outputSRV),"Native NGX output SRV");
        const float f=1.f/std::tan(73.74f*.00872664625997165f),n=.1f,z=1000.f;
        projection={f/aspect,0,0,0,0,f,0,0,0,0,z/(z-n),1,0,0,-n*z/(z-n),0};
        auto set=[&](const char* k,ID3D11Texture2D* r){params->Set(k,static_cast<ID3D11Resource*>(r));};
        // Both input tags reference the same read-only color. Output is a separate allocation.
        set("Latewarp.Backbuffer",color.Get());set("Latewarp.HudlessColor",color.Get());set("Latewarp.UIColorAlpha",ui.Get());
        set("Depth",depth.Get());set("MotionVectors",motion.Get());set("Output",output.Get());set("Latewarp.NoWarpMask",mask.Get());
        for(const char* key:{"Latewarp.Backbuffer","Latewarp.HudlessColor","Latewarp.UIColorAlpha","Latewarp.Output","Latewarp.Depth","Latewarp.MV","Latewarp.NoWarpMask"}){
            const std::string k=key;params->Set((k+".Subrect.Base.X").c_str(),0u);params->Set((k+".Subrect.Base.Y").c_str(),0u);params->Set((k+".Subrect.Width").c_str(),w);params->Set((k+".Subrect.Height").c_str(),h);
        }
        params->Set("Latewarp.WorldToViewMatrix",static_cast<void*>(target.data()));params->Set("Latewarp.ViewToClipMatrix",static_cast<void*>(projection.data()));
        params->Set("Latewarp.PrevRenderedWorldToViewMatrix",static_cast<void*>(source.data()));params->Set("Latewarp.PrevRenderedViewToClipMatrix",static_cast<void*>(projection.data()));
        params->Set("Latewarp.DepthInverted",0u);params->Set("Latewarp.EvalFlags",0u);params->Set("Latewarp.UsePremultiplyUIAlpha",0u);
        ctx->ClearState();initialized=true;
    }
    void updateMask(UINT radius,UINT feather){
        if(radius==radiusCache&&feather==featherCache)return;
        std::vector<uint8_t> bytes(size_t(w)*h,0);
        if(radius||feather){const float r=float(radius),f=float(feather),outer=r+f,cx=float(w)*.5f,cy=float(h)*.5f;
            const UINT x0=UINT(std::max(0.f,std::floor(cx-outer))),x1=UINT(std::min(float(w),std::ceil(cx+outer)));
            const UINT y0=UINT(std::max(0.f,std::floor(cy-outer))),y1=UINT(std::min(float(h),std::ceil(cy+outer)));
            for(UINT y=y0;y<y1;++y)for(UINT x=x0;x<x1;++x){const float dx=float(x)+.5f-cx,dy=float(y)+.5f-cy,d=std::sqrt(dx*dx+dy*dy);
                float a=0.f;if(d<=r)a=1.f;else if(f>0.f&&d<outer){const float t=(d-r)/f;a=1.f-t*t*(3.f-2.f*t);}
                bytes[size_t(y)*w+x]=uint8_t(std::lround(a*255.f));}}
        ctx->UpdateSubresource(mask.Get(),0,nullptr,bytes.data(),w,0);radiusCache=radius;featherCache=feather;
    }
    void evaluate(UINT radius,UINT feather){
        stage(L"Native DX11 NGX center mask and frame registration");
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        updateMask(radius,feather);
        source=camera(yaw,pitch);target=source;
        // Register the real source first. Community NGX behavior indicates
        // a rendered-frame evaluation may ignore the target transform.
        params->Set("Latewarp.IsRenderedFrame",1u);params->Set("Latewarp.FrameID",++frameID);
        ngx(NVSDK_NGX_D3D11_EvaluateFeature(ctx.Get(),feature,params,nullptr),"Native NGX DX11 register frame");
        ctx->ClearState();
        MSG msg{};for(unsigned k=0;k<32&&PeekMessageW(&msg,nullptr,WM_INPUT,WM_INPUT,PM_REMOVE);++k)DispatchMessageW(&msg);
        target=camera(angle(mouseX.load()),angle(mouseY.load()));
        params->Set("Latewarp.IsRenderedFrame",0u);params->Set("Latewarp.FrameID",++frameID);
        stage(L"Native DX11 NGX warp evaluation");
        ngx(NVSDK_NGX_D3D11_EvaluateFeature(ctx.Get(),feature,params,nullptr),"Native NGX DX11 warp evaluation");
        // NGX is allowed to alter immediate-context state. The caller binds all final draw state.
        ctx->ClearState();
        stage(L"Native DX11 NGX output presentation");
    }
    ~Presenter(){if(feature)NVSDK_NGX_D3D11_ReleaseFeature(feature);if(params)NVSDK_NGX_D3D11_DestroyParameters(params);if(sdkReady)NVSDK_NGX_D3D11_Shutdown1(device.Get());}
};
}
