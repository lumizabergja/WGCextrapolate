#pragma once
#include "cs2_mouse.h"

struct ExtrapolationParams {
    float horizon, invW, invH, maxMotion;
    float flowW, flowH, grid, padding;
    float centerRadiusPx, centerFeatherPx, outputW, outputH;
    cs2::CameraMotion camera;
};
static_assert(sizeof(ExtrapolationParams)==80);
static_assert(offsetof(ExtrapolationParams,camera)==48);

inline constexpr char extrapolationShader[]=R"HLSL(
Texture2D<float4> currentFrame:register(t0);
Texture2D<int2> rawFlow:register(t1);
Texture2D<float4> previousFrame:register(t2);
SamplerState samp:register(s0);
cbuffer Settings:register(b0){float horizon;float2 motionInv;float maxMotion;float2 gridExtent;float gridSize;float _padding1;float centerRadiusPx;float centerFeatherPx;float2 outputSize;float4 liveRotation;float4 previousRotation;};
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
// Inverse-project on the capture aspect before stretching to the monitor.
float2 rotateUV(float2 uv,float4 rot){
    float2 tanHalfFov=float2(.75*motionInv.y/motionInv.x,.75);
    float2 ray=(2*uv-1)*tanHalfFov;
    float z=rot.w-rot.z*ray.y;
    float2 xy=float2(rot.y*ray.x+rot.x*z,rot.w*ray.y+rot.z);
    z=rot.y*z-rot.x*ray.x;
    return .5+.5*xy/(max(z,.05)*tanHalfFov);
}
bool insideImage(float2 uv){return all(uv>=0)&&all(uv<=1);}
float4 PS(V i):SV_Target {
    float2 uv=i.uv;
    float4 base=currentFrame.SampleLevel(samp,uv,0);
    [branch] if(i.scene>0.9)return base;
    float mask=centerMask(uv);
    [branch] if(mask==0.0)return base;
    float2 cameraUV=uv;
    float4 cameraColor=base;
    [branch] if(any(liveRotation.xz!=0)){
        cameraUV=rotateUV(uv,liveRotation);
        if(!insideImage(cameraUV)||(centerRadiusPx>0&&centerMask(cameraUV)==0))return base;
        cameraColor=currentFrame.SampleLevel(samp,cameraUV,0);
    }
    float4 cameraOnly=lerp(base,cameraColor,mask);
    [branch] if(horizon<=0.00001)return cameraOnly;
    float2 v=flow(cameraUV);
    float2 residualFlow=v;
    [branch] if(any(previousRotation.xz!=0)){
        float2 priorTurn=rotateUV(cameraUV,previousRotation)-cameraUV;
        // Subtract only camera motion supported by this block's measured flow.
        // Work in source pixels so the match is independent of aspect ratio.
        float2 pixels=priorTurn/motionInv;
        float agreement=saturate(dot(v/motionInv,pixels)/max(dot(pixels,pixels),.0001));
        residualFlow-=agreement*priorTurn;
    }
    float2 q=cameraUV+horizon*residualFlow;
    if(!insideImage(q))return cameraOnly;
    if(centerRadiusPx>0 && (any(liveRotation.xz!=0)||any(previousRotation.xz!=0)) && centerMask(q)==0)return cameraOnly;
    float2 mv=flow(q),old=q+mv;
    if(!insideImage(old)||length(mv)>maxMotion)return cameraOnly;
    float4 next=currentFrame.SampleLevel(samp,q,0);
    float3 prev=previousFrame.SampleLevel(samp,old,0).rgb;
    float residual=dot(abs(next.rgb-prev),float3(.333333,.333333,.333333));
    float confidence=(1-smoothstep(.045,.20,residual))*(1-smoothstep(.003,.025,length(mv-v)));
    return lerp(base,lerp(cameraColor,next,saturate(confidence)),mask);
}
)HLSL";
