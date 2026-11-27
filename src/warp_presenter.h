#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include "late_input.h"
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
#include <cstring>
namespace fgwarp {
using Microsoft::WRL::ComPtr;
inline std::atomic<bool> requested{false};
inline void (*stageCallback)(const wchar_t*){};
inline void stage(const wchar_t* message) noexcept {if(stageCallback)stageCallback(message);}
inline thread_local fglate::History inputHistory;
inline thread_local bool acceptInput=false;
inline int64_t stamp(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
inline void resetInput(){inputHistory.reset(stamp());}
inline void rawInput(LPARAM lp){
    RAWINPUT data{};UINT n=sizeof(data);
    if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&data,&n,sizeof(RAWINPUTHEADER))==UINT(-1))return;
    if(acceptInput && data.header.dwType==RIM_TYPEMOUSE && !(data.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE))
        inputHistory.push(stamp(),data.data.mouse.lLastX,data.data.mouse.lLastY);
}
inline void drainInput(){MSG msg{};for(unsigned i=0;i<128&&PeekMessageW(&msg,nullptr,WM_INPUT,WM_INPUT,PM_REMOVE);++i)DispatchMessageW(&msg);}
struct Handle {HANDLE value{};~Handle(){if(value)CloseHandle(value);}};
inline void check(HRESULT r,const char* name){if(FAILED(r)){char s[160];std::snprintf(s,sizeof(s),"%s (0x%08X)",name,unsigned(r));throw std::runtime_error(s);}}
inline void ngx(NVSDK_NGX_Result r,const char* name){if(!NVSDK_NGX_SUCCEED(r)){char s[160];std::snprintf(s,sizeof(s),"%s (NGX 0x%08X)",name,unsigned(r));throw std::runtime_error(s);}}
struct Presenter {
    bool initialized{},sdkReady{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> color,output,depth,motion,ui,mask,history;
    ComPtr<ID3D11ShaderResourceView> colorSRV,historySRV,depthSRV,motionSRV;
    ComPtr<ID3D11RenderTargetView> maskRTV;
    ComPtr<ID3D11VertexShader> maskVS;
    ComPtr<ID3D11PixelShader> maskPS;
    ComPtr<ID3D11Buffer> maskConstants;
    bool historyValid=false; uint64_t historySequence=0;
    int64_t historyTime=0;
    ComPtr<ID3D11RenderTargetView> inputRTV,depthRTV,motionRTV;
    ComPtr<ID3D11ShaderResourceView> outputSRV;
    NVSDK_NGX_Parameter* params{};
    NVSDK_NGX_Handle* feature{};
    UINT w{},h{},radiusCache{UINT_MAX},featherCache{UINT_MAX},frameID{};
    float aspect{},yaw{},pitch{};
    std::array<float,16> source{},target{},projection{};
    static constexpr double radiansPerCount=-.022*1.5*3.141592653589793/180.;
    static float angle(long long n){return float(std::remainder(n*radiansPerCount,6.283185307179586));}
    static std::array<float,16> camera(float y,float p){float cy=std::cos(y),sy=std::sin(y),cp=std::cos(p),sp=std::sin(p);return {cy,sy*sp,-sy*cp,0,0,cp,sp,0,sy,-cy*sp,cy*cp,0,0,0,0,1};}

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
        depth=tex(DXGI_FORMAT_R32_FLOAT,sr|D3D11_BIND_RENDER_TARGET,depths.data(),w*4);
        check(device->CreateRenderTargetView(depth.Get(),nullptr,&depthRTV),"AI depth RTV");
        motion=tex(DXGI_FORMAT_R16G16_FLOAT,sr|D3D11_BIND_RENDER_TARGET,zeros.data(),w*4);
        check(device->CreateRenderTargetView(motion.Get(),nullptr,&motionRTV),"AI motion RTV");
        check(device->CreateShaderResourceView(depth.Get(),nullptr,&depthSRV),"Depth mask SRV");
        check(device->CreateShaderResourceView(motion.Get(),nullptr,&motionSRV),"Motion mask SRV");
        ui=tex(DXGI_FORMAT_R8G8B8A8_UNORM,sr,zeros.data(),w*4);
        mask=tex(DXGI_FORMAT_R8_UNORM,sr|D3D11_BIND_RENDER_TARGET);
        history=tex(DXGI_FORMAT_R8G8B8A8_UNORM,sr);
        check(device->CreateShaderResourceView(color.Get(),nullptr,&colorSRV),"Mask current color SRV");
        check(device->CreateShaderResourceView(history.Get(),nullptr,&historySRV),"Mask history SRV");
        check(device->CreateRenderTargetView(mask.Get(),nullptr,&maskRTV),"Mask RTV");
        compileMask();
        const float zero[4]{};ctx->ClearRenderTargetView(maskRTV.Get(),zero);
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
    void compileMask(){
        static const char* shader=R"HLSL(
Texture2D<float4> currentImage : register(t0);
Texture2D<float4> previousImage : register(t1);
Texture2D<float> sceneDepth : register(t2);
Texture2D<float2> sceneMotion : register(t3);
cbuffer Config : register(b0) {
    float2 size; float2 shift;
    float hud; float gun; float radius; float feather;
    float valid; float pad0; float pad1; float pad2;
};
float4 VS(uint i:SV_VertexID):SV_Position {
    return float4(i==2?3.0:-1.0,i==1?3.0:-1.0,0,1);
}
float3 at(Texture2D<float4> img,float2 p){
    return img.Load(int3(int2(clamp(p,float2(0,0),size-1)),0)).rgb;
}
float diff(float3 a,float3 b){return dot(abs(a-b),float3(.299,.587,.114));}
float PS(float4 pos:SV_Position):SV_Target {
    float2 p=pos.xy, uv=p/size;
    float3 c=at(currentImage,p);
    float edge=max(diff(c,at(currentImage,p+float2(1,0))),diff(c,at(currentImage,p+float2(0,1))));
    // The camera shift is only an estimate; compare both its sign candidates.
    float screenError=diff(c,at(previousImage,p));
    float sceneError=min(diff(c,at(previousImage,p+shift)),diff(c,at(previousImage,p-shift)));
    float move=smoothstep(1.5,5.0,length(shift))*valid;
    float stable=(1-smoothstep(.025,.12,screenError))*smoothstep(.025,.14,sceneError-screenError);
    float neighborhood=0;
    [unroll] for(int y=-2;y<=2;y+=2){
       [unroll] for(int x=-2;x<=2;x+=2){
          float2 q=p+float2(x,y);float3 a=at(currentImage,q);
          float d=diff(a,at(previousImage,q));
          float e=min(diff(a,at(previousImage,q+shift)),diff(a,at(previousImage,q-shift)));
          neighborhood=max(neighborhood,(1-smoothstep(.025,.12,d))*smoothstep(.025,.14,e-d));
       }
    }
    // HUD: coherent screen-space edges, not a protected rectangle.
    float hudMask=hud*move*max(stable,neighborhood*.8)*smoothstep(.012,.12,edge+neighborhood*.07);
    // Weapon: lower-screen spatial prior only gates detection; no fixed area
    // is protected without independent-motion evidence at the pixel.
    float prior=smoothstep(.48,.77,uv.y)*smoothstep(.08,.25,abs(uv.x-.5)+.10);
    float weaponMotion=smoothstep(.055,.22,sceneError)*smoothstep(.015,.12,edge+screenError*.5);
    // A near-depth prior plus independent motion strengthens the existing
    // conservative foreground mask. Not a trained semantic classifier.
    float nearEvidence=1-smoothstep(.90,.985,sceneDepth.Load(int3(int2(p),0)));
    float2 mv=sceneMotion.Load(int3(int2(p),0));
    float independent=smoothstep(1.0,8.0,min(length(mv-shift),length(mv+shift)));
    float weaponMask=gun*move*prior*max(stable*.8,weaponMotion)*(.45+.55*max(nearEvidence,independent));
    float d=length(p-size*.5);
    float center=(radius+feather>0)?(1-smoothstep(radius,max(radius+.001,radius+feather),d)):0;
    return saturate(max(center,max(hudMask,weaponMask)));
}
)HLSL";
        ComPtr<ID3DBlob> vs,ps,err;
        auto compile=[&](const char* entry,const char* profile,ID3DBlob** blob){
            HRESULT hr=D3DCompile(shader,strlen(shader),"FG auto mask",nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob,&err);
            if(FAILED(hr))throw std::runtime_error(err?static_cast<const char*>(err->GetBufferPointer()):"Compile automatic mask");
        };
        compile("VS","vs_5_0",vs.GetAddressOf());compile("PS","ps_5_0",ps.GetAddressOf());
        check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&maskVS),"Create mask VS");
        check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&maskPS),"Create mask PS");
        D3D11_BUFFER_DESC d{};d.ByteWidth=48;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&d,nullptr,&maskConstants),"Create mask constants");
    }
    void resetHistory(){historyValid=false;historySequence=0;historyTime=0;}
    void updateMask(UINT radius,UINT feather,bool hud,bool gun,uint64_t sequence,int64_t sourceTime){
        // Keep detection on the real-frame cadence. Generated images use the
        // latest inferred per-pixel mask, never a frozen historical color region.
        if(historySequence==sequence && radiusCache==radius && featherCache==feather && lastHud==hud && lastGun==gun)return;
        fglate::Counts a{},b{};
        bool valid=historyValid && historyTime<sourceTime && inputHistory.at(historyTime,a)&&inputHistory.at(sourceTime,b);
        constexpr double rad=.022*1.5*3.14159265358979323846/180.;
        const float focal=float(h)*projection[5]*.5f;
        const float shiftX=valid?float(std::clamp((b.x-a.x)*rad*focal,-double(w)*.12,double(w)*.12)):0;
        const float shiftY=valid?float(std::clamp((b.y-a.y)*rad*focal,-double(h)*.12,double(h)*.12)):0;
        const float data[12]={float(w),float(h),shiftX,shiftY,float(hud),float(gun),float(radius),float(feather),float(valid),0,0,0};
        ctx->UpdateSubresource(maskConstants.Get(),0,nullptr,data,0,0);
        D3D11_VIEWPORT viewport{0,0,float(w),float(h),0,1};
        ctx->RSSetViewports(1,&viewport);
        ctx->OMSetRenderTargets(1,maskRTV.GetAddressOf(),nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(maskVS.Get(),nullptr,0);ctx->PSSetShader(maskPS.Get(),nullptr,0);
        ctx->PSSetConstantBuffers(0,1,maskConstants.GetAddressOf());
        ID3D11ShaderResourceView* views[]={colorSRV.Get(),historyValid?historySRV.Get():colorSRV.Get(),depthSRV.Get(),motionSRV.Get()};
        ctx->PSSetShaderResources(0,4,views);ctx->Draw(3,0);
        ctx->ClearState();
        if(historySequence!=sequence){
            ctx->CopyResource(history.Get(),color.Get());historyValid=true;historySequence=sequence;historyTime=sourceTime;
        }
        radiusCache=radius;featherCache=feather;lastHud=hud;lastGun=gun;
    }
    bool lastHud{},lastGun{};
    void evaluate(UINT radius,UINT feather,bool hud,bool gun,uint64_t sequence,int64_t previousTime,int64_t sourceTime,double horizon,double ticksPerMs){
        ctx->ClearState();
        updateMask(radius,feather,hud,gun,sequence,sourceTime);
        // Every submitted image is a new NGX source, including CUDA-generated
        // images. A second evaluation applies the late pose to that source.
        // Relative depth and backward pixel-space motion have been drawn
        // into the guidance textures. Conventions follow NVIDIA's sample.
        fglate::Counts prev{},cur{};
        const bool sampled=inputHistory.at(sourceTime,cur);
        const bool havePair=sampled && previousTime<sourceTime && inputHistory.at(previousTime,prev);
        const double hx=havePair?horizon*(cur.x-prev.x):0.;
        const double hy=havePair?horizon*(cur.y-prev.y):0.;
        constexpr double rad=-.022*1.5*3.14159265358979323846/180.;
        // Local relative pose: reset source to identity for each new NGX input.
        // Subtract the camera motion already estimated by extrapolation.
        const double sourceYaw=sampled?(cur.x+hx)*rad:0.;
        const double sourcePitch=sampled?std::clamp((cur.y+hy)*rad,-1.55334,1.55334):0.;
        source=camera(float(std::remainder(sourceYaw,6.283185307179586)),float(sourcePitch));target=source;
        params->Set("Latewarp.IsRenderedFrame",1u);params->Set("Latewarp.FrameID",++frameID);
        ngx(NVSDK_NGX_D3D11_EvaluateFeature(ctx.Get(),feature,params,nullptr),"Native NGX DX11 register image");
        ctx->ClearState();
        drainInput();
        const auto now=stamp();fglate::Counts latest{};
        if(sampled && inputHistory.at(now,latest) && now>=sourceTime){
            const auto future=inputHistory.predict(now,ticksPerMs,5.);
            double dx=(latest.x-cur.x+future.x-hx)*rad,dy=(latest.y-cur.y+future.y-hy)*rad;
            const double limit=4.*3.14159265358979323846/180.;
            const double scale=std::max(1.,std::hypot(dx,dy)/limit);
            target=camera(float(std::remainder(sourceYaw+dx/scale,6.283185307179586)),float(std::clamp(sourcePitch+dy/scale,-1.55334,1.55334)));
        }
        params->Set("Latewarp.IsRenderedFrame",0u);params->Set("Latewarp.FrameID",++frameID);
        ngx(NVSDK_NGX_D3D11_EvaluateFeature(ctx.Get(),feature,params,nullptr),"Native NGX DX11 warp evaluation");
        ctx->ClearState();
    }
    ~Presenter(){if(feature)NVSDK_NGX_D3D11_ReleaseFeature(feature);if(params)NVSDK_NGX_D3D11_DestroyParameters(params);if(sdkReady)NVSDK_NGX_D3D11_Shutdown1(device.Get());}
};
}
