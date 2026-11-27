#include "pixel_types.h"

// Full-range BT.601 luma from NvFBC's little-endian BGRA pixels. CUDA NvOFA
// consumes one byte per pixel. The original color buffers feed presentation.
extern "C" __global__ void stageFlowInput(const unsigned* source, unsigned srcPitchPixels,
                                          unsigned char* destination, unsigned dstPitchBytes,
                                          unsigned width, unsigned height) {
    unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    unsigned pixel = source[y * srcPitchPixels + x];
    unsigned blue = pixel & 255u, green = (pixel >> 8) & 255u, red = (pixel >> 16) & 255u;
    unsigned luma = (19595u*red + 38470u*green + 7471u*blue + 32768u) >> 16;
    destination[y * dstPitchBytes + x] = static_cast<unsigned char>(luma);
}

struct Color { float b, g, r, a; };
__device__ __forceinline__ Color unpack(unsigned p) {
    return {float(p & 255), float((p >> 8) & 255), float((p >> 16) & 255), float(p >> 24)};
}
__device__ __forceinline__ unsigned pack(Color c) {
    return __float2uint_rn(c.b) | (__float2uint_rn(c.g) << 8) |
           (__float2uint_rn(c.r) << 16) | (__float2uint_rn(c.a) << 24);
}
__device__ __forceinline__ float mixValue(float a, float b, float f) { return fmaf(f, b-a, a); }
__device__ __forceinline__ Color mixColor(Color a, Color b, float f) {
    return {mixValue(a.b,b.b,f), mixValue(a.g,b.g,f), mixValue(a.r,b.r,f), mixValue(a.a,b.a,f)};
}
struct Cell { int lo, hi; float f; };
__device__ __forceinline__ Cell cell(float p, int size) {
    p = fminf(fmaxf(p, 0.0f), float(size-1));
    int lo = int(p);
    return {lo, min(lo+1,size-1), p-float(lo)};
}
__device__ __forceinline__ Color sampleColor(const SourcePair& pair, unsigned long long address,
                                            unsigned pitch, float2 uv) {
#if defined(__CUDA_ARCH__)
    const auto texture=address==pair.current ? pair.currentTexture : pair.previousTexture;
    if(texture){
        const float4 c=tex2D<float4>(static_cast<cudaTextureObject_t>(texture),uv.x,uv.y);
        return {c.x*255.f,c.y*255.f,c.z*255.f,c.w*255.f};
    }
#endif
    const auto* pixels = reinterpret_cast<const unsigned*>(address);
    Cell x = cell(uv.x * pair.width - 0.5f, pair.width);
    Cell y = cell(uv.y * pair.height - 0.5f, pair.height);
    const unsigned* row0 = pixels + y.lo*pitch;
    const unsigned* row1 = pixels + y.hi*pitch;
    Color top = mixColor(unpack(row0[x.lo]),unpack(row0[x.hi]),x.f);
    Color bottom = mixColor(unpack(row1[x.lo]),unpack(row1[x.hi]),x.f);
    return mixColor(top,bottom,y.f);
}

__device__ __forceinline__ float2 flowAt(const SourcePair& pair, float2 uv) {
    // NvOFA vectors are signed S10.5 translations in source pixels. Sample
    // at native 4x4 block centers, including the last partial row or column.
#if defined(__CUDA_ARCH__)
    if(pair.flowTexture){
        const float gx=uv.x*pair.width*0.25f/float((pair.width+3)/4);
        const float gy=uv.y*pair.height*0.25f/float((pair.height+3)/4);
        const float2 v=tex2D<float2>(static_cast<cudaTextureObject_t>(pair.flowTexture),gx,gy);
        return {v.x*(32767.f/(32.f*pair.width)),v.y*(32767.f/(32.f*pair.height))};
    }
#endif
    Cell x = cell(uv.x*pair.width*0.25f-0.5f, int((pair.width+3)/4));
    Cell y = cell(uv.y*pair.height*0.25f-0.5f, int((pair.height+3)/4));
    const short2* flow = reinterpret_cast<const short2*>(pair.flow);
    short2 a=flow[y.lo*pair.flowPitch+x.lo], b=flow[y.lo*pair.flowPitch+x.hi];
    short2 c=flow[y.hi*pair.flowPitch+x.lo], d=flow[y.hi*pair.flowPitch+x.hi];
    float vx=mixValue(mixValue(a.x,b.x,x.f),mixValue(c.x,d.x,x.f),y.f);
    float vy=mixValue(mixValue(a.y,b.y,x.f),mixValue(c.y,d.y,x.f),y.f);
    return {vx/(32.f*pair.width), vy/(32.f*pair.height)};
}
__device__ __forceinline__ bool outside(float2 uv) {
    return uv.x<0 || uv.y<0 || uv.x>1 || uv.y>1;
}
__device__ __forceinline__ float residual(Color a, Color b) {
    return (fabsf(a.r-b.r)+fabsf(a.g-b.g)+fabsf(a.b-b.b))*(.333333f/255.f);
}
__device__ __forceinline__ float smooth(float a, float b, float value) {
    float t=fminf(fmaxf((value-a)/(b-a),0.f),1.f);
    return t*t*(3.f-2.f*t);
}
__device__ __forceinline__ float magnitude(float x, float y) { return sqrtf(x*x+y*y); }

// Same five samples as the V15 vertex shader. Compute once per flow pair.
extern "C" __global__ void sceneGate(SourcePair pair, unsigned* cut) {
    const float2 probes[5]={{.2f,.3f},{.8f,.3f},{.5f,.5f},{.2f,.7f},{.8f,.7f}};
    float sum=0;
    for (int i=0;i<5;++i) {
        float2 uv=probes[i], v=flowAt(pair,uv);
        float2 old={uv.x+v.x,uv.y+v.y};
        if (!outside(old))
            sum+=residual(sampleColor(pair,pair.current,pair.currentPitch,uv),
                          sampleColor(pair,pair.previous,pair.previousPitch,old));
    }
    *cut = smooth(.19f,.32f,sum*.2f)>.9f;
}

__device__ __forceinline__ float protection(float2 uv, OutputImage target, float radius, float feather) {
    if (radius==0 && feather==0) return 1;
    float d=magnitude((uv.x-.5f)*target.width,(uv.y-.5f)*target.height);
    return feather>0 ? smooth(radius,radius+feather,d) : float(d>radius);
}
__device__ __forceinline__ Color predict(const SourcePair& pair, float2 uv, Color base,
                                         float horizon, float centerWeight) {
    if (centerWeight==0) return base;
    float2 v=flowAt(pair,uv);
    float2 q={uv.x+horizon*v.x,uv.y+horizon*v.y};
    if (outside(q)) return base;
    float2 mv=flowAt(pair,q);
    float2 old={q.x+mv.x,q.y+mv.y};
    if (outside(old) || magnitude(mv.x,mv.y)>.12f) return base;
    Color next=sampleColor(pair,pair.current,pair.currentPitch,q);
    Color prev=sampleColor(pair,pair.previous,pair.previousPitch,old);
    float confidence=(1.f-smooth(.045f,.20f,residual(next,prev)))*
                     (1.f-smooth(.003f,.025f,magnitude(mv.x-v.x,mv.y-v.y)));
    return mixColor(base,next,confidence*centerWeight);
}

extern "C" __global__ void renderFrame(SourcePair pair, OutputImage target, float horizon,
                                       float radius, float feather, const unsigned* cut) {
    unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x>=target.width || y>=target.height) return;
    unsigned* destination=reinterpret_cast<unsigned*>(target.pixels)+y*target.width+x;
    bool native=pair.width==target.width && pair.height==target.height;
    bool extrapolate=horizon>.00001f && cut && !*cut && pair.flow && pair.flowPitch;
    const unsigned* source=reinterpret_cast<const unsigned*>(pair.current);
    if (native && !extrapolate) { *destination=source[y*pair.currentPitch+x]; return; }
    float2 uv={(x+.5f)/target.width,(y+.5f)/target.height};
    Color base=native ? unpack(source[y*pair.currentPitch+x]) : sampleColor(pair,pair.current,pair.currentPitch,uv);
    if (extrapolate) base=predict(pair,uv,base,horizon,protection(uv,target,radius,feather));
    *destination=pack(base);
}

// Rotational inverse projection, evaluated directly in the existing output
// kernel. Positive relative mouse counts sample right/down in the old image.
// No second warp pass or intermediate output image.
__device__ __forceinline__ float2 cameraUV(float2 uv,CameraRotation r,float fx,float fy){
    float x=(uv.x-.5f)/fx,y=(uv.y-.5f)/fy;
    float rx=r.cy*x+r.sy,z=r.cy-r.sy*x;
    float ry=r.cp*y+r.sp*z,rz=r.cp*z-r.sp*y;
    if(rz<.15f)rz=.15f;
    return {.5f+fx*rx/rz,.5f+fy*ry/rz};
}
__device__ __forceinline__ float2 displacement(float2 uv,CameraRotation r,float fx,float fy){
    auto q=cameraUV(uv,r,fx,fy);return {q.x-uv.x,q.y-uv.y};
}
__device__ __forceinline__ float2 sub2(float2 a,float2 b){return {a.x-b.x,a.y-b.y};}
__device__ __forceinline__ float norm2(float2 a){return magnitude(a.x,a.y);}
__device__ __forceinline__ float cameraWeight(SourcePair pair,float2 uv,Color base,InputPose pose,bool hasFlow,float2 v,float2 historical){
    if(!pose.history||(!pose.hud&&!pose.gun))return 1.f;
    const float movement=norm2(historical);
    if(movement<.0012f)return 1.f;
    // Evaluate screen-space stability against the predicted background match.
    // The lower-screen prior gates weapon detection, never freezes an area.
    const float2 old={uv.x+historical.x,uv.y+historical.y};
    if(outside(old))return 1.f;
    const float screenError=residual(base,sampleColor(pair,pair.previous,pair.previousPitch,uv));
    const float sceneError=residual(base,sampleColor(pair,pair.previous,pair.previousPitch,old));
    const float staticEvidence=(1.f-smooth(.025f,.12f,screenError))*smooth(.018f,.12f,sceneError-screenError);
    const float motionEvidence=hasFlow?(1.f-smooth(.001f,.008f,norm2(v))):1.f;
    const float hud=pose.hud?staticEvidence*motionEvidence:0.f;
    const float prior=smooth(.49f,.76f,uv.y);
    const float disagreement=hasFlow?smooth(.0025f,.02f,norm2(sub2(v,historical))):0.f;
    const float independent=smooth(.035f,.19f,sceneError)*disagreement;
    const float gun=pose.gun?prior*fmaxf(staticEvidence,independent*.8f):0.f;
    return 1.f-fminf(1.f,fmaxf(hud,gun));
}
extern "C" __global__ void renderInputFrame(SourcePair pair,OutputImage target,float horizon,
                                            float radius,float feather,const unsigned* cut,InputPose pose){
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=target.width||y>=target.height)return;
    const auto* source=reinterpret_cast<const unsigned*>(pair.current);
    unsigned* dest=reinterpret_cast<unsigned*>(target.pixels)+y*target.width+x;
    float2 uv={(x+.5f)/target.width,(y+.5f)/target.height};
    bool native=pair.width==target.width&&pair.height==target.height;
    Color base=native?unpack(source[y*pair.currentPitch+x]):sampleColor(pair,pair.current,pair.currentPitch,uv);
    const bool hasFlow=cut&&!*cut&&pair.flow&&pair.flowPitch;
    // A scene cut or invalid timestamp uses the unchanged V15 path.
    if(!pose.active||(cut&&*cut)){
        if(hasFlow&&horizon>.00001f)base=predict(pair,uv,base,horizon,protection(uv,target,radius,feather));
        *dest=pack(base);return;
    }
    const float center=protection(uv,target,radius,feather);
    if(center<=0.f){*dest=pack(base);return;}
    float2 late=displacement(uv,pose.latest,pose.fx,pose.fy);
    float2 old=pose.history?displacement(uv,pose.historical,pose.fx,pose.fy):float2{0,0};
    float2 v=hasFlow?flowAt(pair,uv):float2{0,0};
    const float weight=cameraWeight(pair,uv,base,pose,hasFlow,v,old);
    // Replace the estimated camera component, keeping local object motion.
    // If history is unavailable, keep original flow extrapolation but omit
    // the additional mouse correction when that would double-count motion.
    if(hasFlow&&horizon>0.f&&!pose.history){
        *dest=pack(predict(pair,uv,base,horizon,center));return;
    }
    const float2 cameraQ={uv.x+late.x*weight,uv.y+late.y*weight};
    // Feather only at the newly exposed image boundary.
    const float edge=smooth(0.f,.015f,fminf(fminf(cameraQ.x,1.f-cameraQ.x),fminf(cameraQ.y,1.f-cameraQ.y)));
    Color camera=outside(cameraQ)?base:mixColor(base,sampleColor(pair,pair.current,pair.currentPitch,cameraQ),edge);
    if(hasFlow&&horizon>.00001f){
        const float2 object={v.x-old.x*weight,v.y-old.y*weight};
        const float2 q={cameraQ.x+horizon*object.x,cameraQ.y+horizon*object.y};
        if(!outside(q)){
            const float2 mv=flowAt(pair,q);
            const float2 prev={q.x+mv.x,q.y+mv.y};
            if(!outside(prev)&&norm2(mv)<.12f){
                const Color candidate=sampleColor(pair,pair.current,pair.currentPitch,q);
                const Color reference=sampleColor(pair,pair.previous,pair.previousPitch,prev);
                const float confidence=(1.f-smooth(.045f,.20f,residual(candidate,reference)))*
                                       (1.f-smooth(.003f,.025f,norm2(sub2(mv,v))));
                camera=mixColor(camera,candidate,confidence*edge);
            }
        }
    }
    *dest=pack(mixColor(base,camera,center));
}
