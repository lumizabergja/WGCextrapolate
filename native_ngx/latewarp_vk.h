#pragma once
// Experimental adapter. Generic public NGX ABI, private parameter semantics
// inferred from public D3D12 community integration and the supplied DLL.
// Not linked into FG; not GPU validated. Caller owns Vulkan resource lifetimes,
// device-extension discovery, NGX initialization, barriers, and synchronization.
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <array>
#include <string>
namespace fg_ngx_experiment {
struct Rect { unsigned x,y,w,h; };
struct Inputs {
    NVSDK_NGX_Resource_VK backbuffer, hudless, ui, depth, motion, output;
    NVSDK_NGX_Resource_VK* mask=nullptr;
    Rect color{}, depthRect{}, maskRect{};
    std::array<float,16> targetView{}, targetProjection{}, sourceView{}, sourceProjection{};
    bool depthInverted=false;
};
using BetweenEvaluations = void (*)(VkCommandBuffer, void*);
inline void rect(NVSDK_NGX_Parameter* p, const char* prefix, Rect r) {
    const std::string k(prefix);
    p->Set((k+".Subrect.Base.X").c_str(),r.x);
    p->Set((k+".Subrect.Base.Y").c_str(),r.y);
    p->Set((k+".Subrect.Width").c_str(),r.w);
    p->Set((k+".Subrect.Height").c_str(),r.h);
}
inline NVSDK_NGX_Result create(VkCommandBuffer cmd, NVSDK_NGX_Parameter* p,
                              unsigned w,unsigned h,NVSDK_NGX_Handle** handle) {
    if(!cmd || !p || !handle || !w || !h) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    p->Set("Latewarp.Output.Width",w); p->Set("Latewarp.Output.Height",h);
    return NVSDK_NGX_VULKAN_CreateFeature(cmd,NVSDK_NGX_Feature_Reserved15,p,handle);
}
inline NVSDK_NGX_Result record_real(VkCommandBuffer cmd,NVSDK_NGX_Handle* h,
     NVSDK_NGX_Parameter* p,Inputs& in,unsigned& frameId, bool isReal,
     BetweenEvaluations barrier,void* context) {
    if(!isReal || !cmd || !h || !p || !barrier || !in.color.w || !in.color.h || !in.depthRect.w || !in.depthRect.h)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    p->Set("Latewarp.Backbuffer",static_cast<void*>(&in.backbuffer));
    p->Set("Latewarp.HudlessColor",static_cast<void*>(&in.hudless));
    p->Set("Latewarp.UIColorAlpha",static_cast<void*>(&in.ui));
    p->Set("Depth",static_cast<void*>(&in.depth));
    p->Set("MotionVectors",static_cast<void*>(&in.motion));
    p->Set("Output",static_cast<void*>(&in.output));
    p->Set("Latewarp.NoWarpMask",static_cast<void*>(in.mask));
    for(auto k:{"Latewarp.Backbuffer","Latewarp.HudlessColor","Latewarp.UIColorAlpha","Latewarp.Output"}) rect(p,k,in.color);
    rect(p,"Latewarp.Depth",in.depthRect); rect(p,"Latewarp.MV",in.depthRect);
    if(in.mask) rect(p,"Latewarp.NoWarpMask",in.maskRect);
    p->Set("Latewarp.WorldToViewMatrix",static_cast<void*>(in.targetView.data()));
    p->Set("Latewarp.ViewToClipMatrix",static_cast<void*>(in.targetProjection.data()));
    p->Set("Latewarp.PrevRenderedWorldToViewMatrix",static_cast<void*>(in.sourceView.data()));
    p->Set("Latewarp.PrevRenderedViewToClipMatrix",static_cast<void*>(in.sourceProjection.data()));
    p->Set("Latewarp.DepthInverted",in.depthInverted?1u:0u); p->Set("Latewarp.EvalFlags",0u);
    p->Set("Latewarp.IsRenderedFrame",1u); p->Set("Latewarp.FrameID",++frameId);
    auto r=NVSDK_NGX_VULKAN_EvaluateFeature(cmd,h,p,nullptr);
    if(!NVSDK_NGX_SUCCEED(r)) return r;
    barrier(cmd,context);
    p->Set("Latewarp.IsRenderedFrame",0u); p->Set("Latewarp.FrameID",++frameId);
    return NVSDK_NGX_VULKAN_EvaluateFeature(cmd,h,p,nullptr);
}
}
